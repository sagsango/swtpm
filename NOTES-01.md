# swtpm main flow
┌────────────────────────────────────────────┐
│                swtpm main()                │
│                (main.c)                    │
└────────────────────────────────────────────┘
                │
                ▼
      ┌─────────────────────────────────────┐
      │ Parse argv[1]  → interface type     │
      │   "socket" | "chardev" | "cuse"     │
      └─────────────────────────────────────┘
                │
 ┌──────────────┼───────────────────────────────┐
 ▼              ▼                               ▼
socket mode   chardev mode                    cuse mode
 │              │                               │
 │              │                               │
 ▼              ▼                               ▼
swtpm_main()  swtpm_chardev_main()        swtpm_cuse_main()
(main.c)      (main_chardev.c)            (main_cuse.c)
 │              │                               │
 │              │                               │
 ▼              ▼                               ▼
┌────────────────────────────────────────────┐
│         Phase 1 — Command-line parse       │
└────────────────────────────────────────────┘
│
├─ getopt_long():
│     --server type=[unixio|tcp],...
│     --ctrl type=[unixio|tcp],...
│     --tpmstate dir=...
│     --key, --migration-key
│     --log, --pid, --flags, --locality
│     --tpm2, --runas, --daemon
│
└─ Results stored into local vars:
      keydata, tpmstatedata, serverdata, ctrlchdata, ...
      → passed into handle_*_options()

                │
                ▼
┌────────────────────────────────────────────┐
│        Phase 2 — Environment setup         │
└────────────────────────────────────────────┘
│
├─ handle_ctrlchannel_options() → create control socket
│    → returns struct ctrlchannel *
│
├─ handle_server_options() → parse server params, open socket
│    → unixio_open_socket() / tcp_open_socket()
│    → server_new(fd, flags, path)   (server.c)
│
├─ handle_log_options() → open log file (logging.c)
│
├─ handle_key_options() / handle_migration_key_options()
│    → setup AES keys for state encryption (swtpm_aes.c)
│
├─ handle_tpmstate_options() → set NVRAM directory (tpmstate.c)
│
├─ handle_flags_options() → parse operational flags
│
├─ handle_locality_options() → set allowed TPM localities
│
├─ change_process_owner() (utils.c)
│    → setuid(), setgid(), initgroups() to drop privileges
│
├─ pidfile_write(getpid()) → record PID
│
└─ If --daemon → daemon(0,0) or osx_daemon() (background mode)

                │
                ▼
┌────────────────────────────────────────────┐
│     Phase 3 — TPM Engine Initialization    │
└────────────────────────────────────────────┘
│
├─ TPMLIB_ChooseTPMVersion()
│      → Select TPM 1.2 or TPM 2.0 core
│
├─ SWTPM_NVRAM_Set_TPMVersion()
│      → configure NVRAM backend version
│
├─ tpmlib_register_callbacks(&callbacks)
│      ↳ libtpms calls these for:
│         • SWTPM_NVRAM_Init / Load / Store / Delete
│         • SWTPM_IO_Init
│         • mainloop_cb_get_locality
│
├─ If !need_init_cmd:
│      tpmlib_start(0, version)
│      → powers up internal TPM engine
│      → allocates persistent state memory
│
└─ tpmlib_debug_libtpms_parameters()
       → log compile-time TPM capabilities (utils.c)

                │
                ▼
┌────────────────────────────────────────────┐
│      Phase 4 — Signal + Thread Setup       │
└────────────────────────────────────────────┘
│
├─ install_sighandlers(notify_fd, sigterm_handler)
│     → creates pipe[2] for SIGTERM notifications
│     → installs handler:
│          sigterm_handler(): write(notify_fd[1],"T")
│
├─ threadpool_init()  (threadpool.c)
│     → g_mutex_init(thread_busy_lock)
│     → g_cond_init(thread_busy_signal)
│     → create GThreadPool with 1 TPM thread
│
└─ file_ops_lock setup for NVRAM I/O serialization

                │
                ▼
┌────────────────────────────────────────────┐
│           Phase 5 — Main Event Loop        │
│              (mainloop.c)                  │
└────────────────────────────────────────────┘
│
│ mainLoop(&mlp, notify_fd[0])
│   Parameters:
│     mlp.fd = TPM socket (from server.c)
│     mlp.cc = control channel
│     mlp.flags = runtime behavior bits
│
│ Polls over:
│   ├── TPM command socket
│   ├── Control channel socket
│   ├── notify_fd[0] (for SIGTERM)
│
│  while (!mainloop_terminate) {
│       poll()
│       if (TPM command data) → handle_tpm_command()
│       if (control message)  → handle_ctrlcommand()
│       if (notify_fd readable) → shutdown
│  }

                │
                ▼
┌────────────────────────────────────────────┐
│         Phase 6 — TPM Command Handling     │
└────────────────────────────────────────────┘
│
│ handle_tpm_command():
│     ├─ read() from TPM socket
│     ├─ worker_thread_mark_busy()
│     ├─ dispatch to TPM worker thread:
│         g_thread_pool_push(process_tpm_cmd)
│
│ process_tpm_cmd():
│     ├─ tpmlib_process_command(inbuf)
│     │     → calls libtpms core:
│     │         TPM_ProcessCommand()
│     │             ├── TPM command parser
│     │             ├── crypto (tpm_crypto_freebl.c)
│     │             ├── state mgmt (tpm_nvram.c)
│     │             └── generates TPM response
│     ├─ worker_thread_mark_done()
│     └─ send() response to client
│
│ Control flow within libtpms:
│     ┌───────────────────────────────────────┐
│     │ TPM_ProcessCommand()                 │
│     │   ├─ Parse command header            │
│     │   ├─ Dispatch to TPM command handler │
│     │   │     e.g. TPM_LoadKey, TPM_Seal   │
│     │   ├─ Update TPM state structures     │
│     │   └─ Return response buffer          │
│     └───────────────────────────────────────┘
│
│ After command:
│   ├─ mainloop writes response to socket
│   ├─ thread_busy → false (condvar signal)
│   └─ back to poll()

                │
                ▼
┌────────────────────────────────────────────┐
│        Phase 7 — Control Channel Flow      │
└────────────────────────────────────────────┘
│
│ Control socket handles meta-commands:
│   - INIT       → start TPM (tpmlib_start)
│   - STOP       → terminate TPM
│   - SAVESTATE  → persist NVRAM
│   - RESET      → clear TPM state
│   - GET_CAPS   → query TPM info
│
│ These commands are handled by ctrlchannel.c
│ which internally calls libtpms APIs via tpmlib.c

                │
                ▼
┌────────────────────────────────────────────┐
│           Phase 8 — Termination            │
└────────────────────────────────────────────┘
│
│ On SIGTERM:
│    sigterm_handler() writes to notify_fd[1]
│    → mainLoop() sees notify_fd[0] readable
│    → sets mainloop_terminate = true
│
│ mainLoop() exits
│
│ TPMLIB_Terminate()
│     ├─ flush volatile state
│     ├─ NVRAM flush to disk
│     └─ free TPM structures
│
│ swtpm_cleanup():
│     ├─ pidfile_remove()
│     ├─ ctrlchannel_free()
│     ├─ server_free() → close/unlink socket
│     ├─ log_global_free()
│     └─ tpmstate_global_free()
│
│ threadpool_end() → g_thread_pool_free()
│
└─ Exit(EXIT_SUCCESS / EXIT_FAILURE)

# Supporting Subsystem Hierarchy
+---------------------------------------------------+
|                swtpm Frontend                     |
|  main.c / swtpm_main.c / swtpm_chardev_main.c     |
|---------------------------------------------------|
|   ↓ init                                          |
|   utils.c (signals, user, fd resolve)             |
|   logging.c (log files, levels)                   |
|   server.c (socket mgmt)                          |
|   ctrlchannel.c (mgmt interface)                  |
|   mainloop.c (poll loop)                          |
|   threadpool.c (TPM execution thread)             |
|   swtpm_nvfile.c (NVRAM backend)                  |
|---------------------------------------------------|
|                libtpms Core                       |
|   tpm_library.c / tpm_memory.c / tpm_nvram.c      |
|   tpm_crypto_freebl.c (AES, SHA, RSA)             |
|   tpm_data_structures.c                           |
|   tpm_commands.c (TPM command handlers)           |
|---------------------------------------------------|
|                Host OS                            |
|   syscalls: poll, read, write, select, pipe       |
|   signals: SIGTERM, SIGPIPE                       |
+---------------------------------------------------+
