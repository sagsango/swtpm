┌────────────────────────────────────────────┐
│                swtpm main()                │
│            (src/swtpm/main.c)              │
└────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────┐
│  Parse argv[1] interface type:             │
│     socket | chardev | cuse                │
└────────────────────────────────────────────┘
                │
   ┌────────────┼────────────┐
   ▼            ▼            ▼
swtpm_main() swtpm_chardev_main() swtpm_cuse_main()
   │            │            │
   └────── Common initialization ───────────┘
                │
                ▼
┌────────────────────────────────────────────┐
│       (1) Command-line + Environment setup │
└────────────────────────────────────────────┘
│
│ handle_ctrlchannel_options() → create control socket
│ handle_server_options()      → open TPM command socket
│ handle_tpmstate_options()    → set NVRAM dir
│ handle_key_options()         → setup AES key (swtpm_aes.c)
│ handle_log_options()         → open logfile
│ change_process_owner()       → drop privileges (utils.c)
│ pidfile_write(getpid())      → record PID
│
└─> produces:
     - struct mainLoopParams mlp
     - struct ctrlchannel *cc
     - struct server *server
     - config for TPM version, NVRAM, keys, flags, etc.

                │
                ▼
┌────────────────────────────────────────────┐
│       (2) libtpms Core Initialization      │
└────────────────────────────────────────────┘
│
│  TPMLIB_ChooseTPMVersion(mlp.tpmversion)
│     └──> Select TPM 1.2 / TPM 2.0 engine
│
│  SWTPM_NVRAM_Set_TPMVersion()   // NVRAM version
│
│  tpmlib_register_callbacks(&callbacks)
│       ┌──────────────────────────────────────────────┐
│       │ struct libtpms_callbacks:                    │
│       │   .tpm_nvram_init        → SWTPM_NVRAM_Init  │
│       │   .tpm_nvram_loaddata    → SWTPM_NVRAM_LoadData│
│       │   .tpm_nvram_storedata   → SWTPM_NVRAM_StoreData│
│       │   .tpm_nvram_deletename  → SWTPM_NVRAM_DeleteName│
│       │   .tpm_io_init           → SWTPM_IO_Init     │
│       │   .tpm_io_getlocality    → mainloop_cb_get_locality│
│       └──────────────────────────────────────────────┘
│
│ These callbacks connect swtpm’s host-side file/socket logic
│ to libtpms’s TPM logic (which is pure C logic, no I/O).
│
│  tpmlib_start(0, version)
│      └──> internally calls TPM_MainInit()
│             ├─ Load persistent state from disk (via SWTPM_NVRAM_LoadData)
│             ├─ Initialize crypto primitives (AES, RSA, SHA)
│             └─ Prepare internal TPM registers and PCR banks
│
│ TPM engine now ready to receive commands
│
└──────────────────────────────────────────────┘

                │
                ▼
┌────────────────────────────────────────────┐
│      (3) Main Event Loop (mainloop.c)      │
└────────────────────────────────────────────┘
│
│ mainLoop(&mlp, notify_fd[0])
│   ├─ poll() over:
│   │   - TPM server socket (fd)
│   │   - Control channel socket
│   │   - notify_fd[0] (signal pipe)
│   │
│   ├─ on incoming TPM command:
│   │     read() → handle_tpm_command()
│   │
│   ├─ on control message:
│   │     handle_ctrlcommand()  (e.g., INIT, SAVE_STATE)
│   │
│   ├─ on SIGTERM (notify_fd readable):
│   │     break loop, terminate gracefully
│   │
│   └─ Loop until mainloop_terminate == true
│
└────────────────────────────────────────────┘
                │
                ▼
┌────────────────────────────────────────────┐
│    (4) TPM Command Handling (threadpool)   │
└────────────────────────────────────────────┘
│
│ handle_tpm_command():
│   ├─ read command bytes into buffer
│   ├─ worker_thread_mark_busy()
│   ├─ g_thread_pool_push(process_tpm_cmd)
│
│ process_tpm_cmd():
│   ├─ Calls tpmlib_process() / tpmlib_process_command()
│   │
│   │  ↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓
│   │  **libtpms side begins here**
│   │
│   │  TPMLIB_ProcessCommand(in, in_len, &out, &out_len)
│   │       └──> TPM_ProcessCommand()
│   │              ├─ Parse TPM command header
│   │              ├─ Verify auth/session state
│   │              ├─ Lookup command handler
│   │              │     (e.g., TPM_LoadKey, TPM_PCRRead)
│   │              ├─ Execute command logic
│   │              │     ↳ Crypto ops (AES, RSA, SHA)
│   │              │     ↳ State updates in TPM structures
│   │              │     ↳ NVRAM calls via SWTPM_NVRAM_StoreData()
│   │              ├─ Serialize response buffer
│   │              └─ Return TPM response
│   │
│   │  **libtpms side ends here**
│   │
│   ├─ worker_thread_mark_done()
│   └─ send(response_fd, out, out_len, 0)
│
│ Control channel messages use tpmlib_start/terminate directly
│ for TPM lifecycle ops, bypassing command path.
│
└────────────────────────────────────────────┘

                │
                ▼
┌────────────────────────────────────────────┐
│     (5) NVRAM, Crypto, and IO Backends     │
└────────────────────────────────────────────┘
│
│ libtpms uses callbacks defined in swtpm:
│
│ ┌──────────────┬────────────────────────────────────────────┐
│ │ Callback     │ Implemented in:                            │
│ ├──────────────┼────────────────────────────────────────────┤
│ │ tpm_nvram_init         │ swtpm_nvfile.c: SWTPM_NVRAM_Init()│
│ │ tpm_nvram_loaddata     │ swtpm_nvfile.c: SWTPM_NVRAM_LoadData()│
│ │ tpm_nvram_storedata    │ swtpm_nvfile.c: SWTPM_NVRAM_StoreData()│
│ │ tpm_nvram_deletename   │ swtpm_nvfile.c: SWTPM_NVRAM_DeleteName()│
│ │ tpm_io_init            │ swtpm_io.c: SWTPM_IO_Init()            │
│ │ tpm_io_getlocality     │ mainloop_cb_get_locality()             │
│ └──────────────┴────────────────────────────────────────────┘
│
│ Each maps libtpms internal operations to host actions:
│   • Persistent state stored in /var/lib/swtpm/<tpmX>
│   • Data optionally encrypted with AES (swtpm_aes.c)
│   • File access protected by mutex (file_ops_lock)
│
└────────────────────────────────────────────┘

                │
                ▼
┌────────────────────────────────────────────┐
│       (6) Signal Handling & Shutdown       │
└────────────────────────────────────────────┘
│
│ SIGTERM → sigterm_handler():
│              write(notify_fd[1], "T")
│              mainloop_terminate = true
│
│ poll() wakes up → exits loop
│
│ TPMLIB_Terminate():
│     ├─ TPM_Terminate() → saves volatile → permanent state
│     ├─ SWTPM_NVRAM_StoreData() flushes files
│
│ swtpm_cleanup():
│     ├─ pidfile_remove()
│     ├─ ctrlchannel_free()
│     ├─ server_free()
│     ├─ log_global_free()
│     └─ tpmstate_global_free()
│
└────────────────────────────────────────────┘

                │
                ▼
┌────────────────────────────────────────────┐
│                Process Exit                │
└────────────────────────────────────────────┘

