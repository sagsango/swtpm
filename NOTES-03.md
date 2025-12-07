# char devce vs cuse
          Traditional Character Device
          ============================

  User process
  ┌────────────────────────┐
  │ open("/dev/ttyS0")     │
  │ write(fd, buf, n)      │
  └────────────────────────┘
             │
             ▼
  ┌────────────────────────┐
  │ Kernel driver (tty.c)  │  ← implements struct file_operations
  │   .open()              │
  │   .write()             │
  │   .ioctl()             │
  └────────────────────────┘
             │
             ▼
        Hardware (UART)



        CUSE (Character Device in Userspace)
        ====================================

  User process (client)
  ┌────────────────────────┐
  │ open("/dev/swtpm0")    │
  │ write(fd, buf, n)      │
  └────────────────────────┘
             │
             ▼
  ┌────────────────────────┐
  │  Kernel CUSE driver    │
  │   (in-kernel FUSE/CUSE)│
  └────────────────────────┘
             │
             ▼
  ┌────────────────────────┐
  │  User-space daemon     │  ← implements CUSE API
  │  (e.g., swtpm_cuse)    │
  │   cuse_lowlevel_ops:   │
  │     .read(), .write()  │
  │     .ioctl()           │
  └────────────────────────┘


# swtpm
             swtpm (daemon)
          ┌──────────────────┐
          │ swtpm_main()     │
          │ swtpm_socket_main() │
          │ swtpm_chardev_main()│
          │ swtpm_cuse_main()   │
          └──────────────────┘
                 │
   ┌─────────────┼─────────────────────┐
   │             │                     │
Socket Mode   CharDev Mode          CUSE Mode
(mainloop.c)  (main_chardev.c)      (main_cuse.c)
   │             │                     │
TCP/UnixIO   open("/dev/tpm0")     cuse_init() → create /dev/tpm0
socket       ↕ Kernel TPM driver   ↕ Kernel CUSE relay
   │             │                     │
   ▼             ▼                     ▼
libtpms     libtpms                libtpms


# swtpm io path
QEMU ----read/write----> /dev/tpm0 ----kernel----> Hardware TPM

QEMU ----read/write----> /dev/tpm0 ----CUSE----> swtpm (user-space TPM)
                                         │
                                         └──> libtpms core logic

# char-device vs cuse
+--------------------------+----------------------------------------+-----------------------------------------------------+
| Feature                  | Character Device (chardev)             | CUSE Device (Character Device in Userspace)        |
+--------------------------+----------------------------------------+-----------------------------------------------------+
| Implemented by           | Kernel driver                          | User-space process (via CUSE library)              |
| Interface path           | /dev/<name>                            | /dev/<name> (via CUSE)                             |
| Implementation source    | Kernel module defines file_operations  | User program uses CUSE (part of FUSE)              |
| System call handling     | Kernel handles open/read/write/ioctl   | Kernel forwards syscalls to user process           |
| User-space integration   | Typically kernel-only                  | Implemented fully in user-space (no kernel code)   |
| Performance              | Fast (direct syscall in kernel)        | Slower (syscalls bounce through FUSE relay)        |
| Example                  | /dev/ttyS0, /dev/random, /dev/kvm      | /dev/swtpm0, /dev/vhost-user-blk0 (emulated)       |
+--------------------------+----------------------------------------+-----------------------------------------------------+

