# chardev flow
══════════════════════════════════════════════════════════════════════════════
        VIRTUAL TPM COMMAND FLOW — GUEST → HOST → SWTPM → LIBTPMS
══════════════════════════════════════════════════════════════════════════════

   ┌───────────────────────────────────────────────────────────────────────┐
   │                Guest VM (running under QEMU/KVM)                      │
   └───────────────────────────────────────────────────────────────────────┘
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  [Guest userspace app]                                       │
   │     e.g., tpm2_getrandom, systemd-cryptsetup, tcsd, etc.     │
   └──────────────────────────────────────────────────────────────┘
             │
             │   open("/dev/tpm0"), write(TPM command buffer)
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │  [Guest kernel driver: tpm_tis / tpm_crb]                    │
   │   - Formats TPM command packets                              │
   │   - Issues IO to /dev/tpm0                                   │
   └──────────────────────────────────────────────────────────────┘
             │
             │   (VM device emulated by QEMU)
             ▼
══════════════════════════════════════════════════════════════════════════════
                       HOST SIDE (QEMU + swtpm)
══════════════════════════════════════════════════════════════════════════════
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ QEMU                                                         │
   │  - Emulates TPM TIS/CRB MMIO registers                       │
   │  - Sends TPM command to host “backend” (emulator)            │
   │                                                              │
   │ Example backend parameter:                                   │
   │   -tpmdev emulator,id=tpm0,chardev=chrtpm                    │
   │   -chardev socket,id=chrtpm,path=/run/swtpm.sock             │
   │   OR                                                         │
   │   -chardev vtpm,id=chrtpm,dev=/dev/vtpmx                     │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ [Kernel module: vtpm_proxy.ko]                               │
   │                                                              │
   │  - Exposes /dev/vtpmx (control)                              │
   │  - ioctl(VTPM_PROXY_IOC_NEW_DEV) → creates /dev/tpmN          │
   │  - Establishes a communication channel                        │
   │       host-kernel ↔ userspace (swtpm) via socketpair          │
   │                                                              │
   │  TPM command bytes from guest:                               │
   │      write(/dev/tpm0)                                        │
   │         → vtpm_proxy → send() → swtpm fd                     │
   │  TPM response:                                               │
   │      swtpm fd → vtpm_proxy → read(/dev/tpm0)                 │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ [swtpm process] — userspace TPM emulator                     │
   │                                                              │
   │ main() → swtpm_chardev_main()                                │
   │    │ open /dev/tpmN or use fd returned by vtpm_proxy          │
   │    │ register callbacks: SWTPM_NVRAM_*, locality, etc.       │
   │    │ tpmlib_register_callbacks(&callbacks)                   │
   │    │ tpmlib_start()                                          │
   │    │ mainLoop(fd)                                            │
   │                                                              │
   │ mainLoop:                                                    │
   │    read(fd) ← TPM command                                    │
   │    ├─ tpmlib_process()                                       │
   │    │     └─ TPMLIB_Process()                                 │
   │    │           ↓                                             │
   │    │        libtpms core executes TPM logic                  │
   │    ├─ write(fd, TPM response)                                │
   │    └─ loop                                                   │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
══════════════════════════════════════════════════════════════════════════════
                        LIBTPMS LAYER (Emulated TPM Core)
══════════════════════════════════════════════════════════════════════════════
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ libtpms (TPMLIB_Process)                                     │
   │                                                              │
   │  - Implements full TPM 1.2 / TPM 2.0 logic                   │
   │  - Interprets command header & opcode                        │
   │  - Calls TPM_xxx() handlers                                  │
   │  - Updates internal state (PCRs, keys, NV indices)           │
   │  - Returns TPM_RESPONSE_HEADER + data                        │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Callbacks to SWTPM_NVRAM_*()                                 │
   │    ├─ SWTPM_NVRAM_LoadData() → read from disk                │
   │    ├─ SWTPM_NVRAM_StoreData() → write updated state          │
   │    └─ SWTPM_NVRAM_DeleteName() → delete volatile blob         │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Host filesystem                                              │
   │   (e.g. /var/lib/swtpm/tpm0/...)                            │
   │   ├─ NVChip file (non-volatile storage)                      │
   │   ├─ Permanent key blobs                                     │
   │   ├─ Volatile state (PCRs, sessions)                         │
   │   └─ Migration blobs                                         │
   └──────────────────────────────────────────────────────────────┘
             │
             ▼
══════════════════════════════════════════════════════════════════════════════
                           REVERSE PATH (Response)
══════════════════════════════════════════════════════════════════════════════
   libtpms → swtpm → vtpm_proxy → QEMU → guest /dev/tpm0 → userspace app
══════════════════════════════════════════════════════════════════════════════

