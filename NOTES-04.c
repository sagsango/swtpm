
/*
 * XXX: mini-qemu program which creates the swtpm and talks to it
 *      using the unix socket
┌────────────────────────────────────┐
│ mini_qemu_tpm.c                   │
│  (our mock hypervisor frontend)   │
└───────────────┬────────────────────┘
                │
                ▼
        fork() + exec("swtpm socket ...")
                │
                ▼
┌────────────────────────────────────┐
│ swtpm                              │
│  listens on /tmp/mytpm.sock        │
│  ↳ uses mainLoop() + tpmlib_*()    │
│  ↳ libtpms backend                 │
└────────────────────────────────────┘
                │
                ▼
connect("/tmp/mytpm.sock")
send(TPM2_GetCapability)
                │
                ▼
swtpm receives → tpmlib_process_command()
   ↳ libtpms executes TPM2 logic
                │
                ▼
swtpm sends back TPM2 response
                │
                ▼
mini_qemu_tpm prints hex dump of response
*/
/*
 * mini_qemu_tpm.c
 * ----------------------------------------------------
 * Minimal QEMU-like program that:
 *   1. Spawns swtpm socket backend
 *   2. Connects to its Unix socket
 *   3. Sends a simple TPM2 command (GetCapability)
 *   4. Prints the response from libtpms
 *
 * Build:  gcc -o mini_qemu_tpm mini_qemu_tpm.c
 * Run:    ./mini_qemu_tpm
 *
 * Requirements:
 *   - swtpm binary in PATH
 *   - libtpms installed
 *   - /tmp writable
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

static const char *TPM_DIR   = "/tmp/mytpm";
static const char *TPM_SOCK  = "/tmp/mytpm.sock";
static const char *CTRL_SOCK = "/tmp/mytpm.ctrl";

/* Simple TPM2_GetCapability command:
 * tag(0x8001) len(0x00000016) cmd(0x0000017A)
 * cap=TPM_CAP_TPM_PROPERTIES (0x00000006)
 * prop=TPM_PT_FIXED (0x00000100)
 * count=1
 */
static const unsigned char tpm_getcap_cmd[] = {
    0x80, 0x01,
    0x00, 0x00, 0x00, 0x16,
    0x00, 0x00, 0x01, 0x7A,
    0x00, 0x00, 0x00, 0x06,
    0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x01
};

/* Utility: wait until socket file exists */
static int wait_for_socket(const char *path, int timeout_ms)
{
    struct stat st;
    int waited = 0;
    while (waited < timeout_ms) {
        if (stat(path, &st) == 0)
            return 0;
        usleep(100000);
        waited += 100;
    }
    return -1;
}

/* Launch swtpm as background process (like QEMU would) */
static pid_t launch_swtpm(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execlp("swtpm", "swtpm",
               "socket",
               "--tpmstate", "dir=/tmp/mytpm",
               "--ctrl", "type=unixio,path=/tmp/mytpm.ctrl",
               "--server", "type=unixio,path=/tmp/mytpm.sock",
               "--flags", "not-need-init",
               "--tpm2",
               (char *)NULL);
        perror("execlp swtpm");
        _exit(127);
    }

    printf("[mini-qemu] spawned swtpm (pid=%d)\n", pid);

    if (wait_for_socket(TPM_SOCK, 3000) != 0) {
        fprintf(stderr, "Timeout waiting for swtpm socket\n");
        kill(pid, SIGTERM);
        return -1;
    }

    printf("[mini-qemu] swtpm socket ready at %s\n", TPM_SOCK);
    return pid;
}

/* Connect to TPM socket and send command */
static void talk_to_tpm(void)
{
    struct sockaddr_un addr;
    unsigned char buf[4096];
    int fd, n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, TPM_SOCK);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return;
    }

    printf("[mini-qemu] sending TPM2_GetCapability...\n");

    if (write(fd, tpm_getcap_cmd, sizeof(tpm_getcap_cmd)) != sizeof(tpm_getcap_cmd)) {
        perror("write");
        close(fd);
        return;
    }

    n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        perror("read");
        close(fd);
        return;
    }

    printf("[mini-qemu] received %d bytes from swtpm:\n", n);
    for (int i = 0; i < n; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }
    printf("\n");

    close(fd);
}

int main(void)
{
    pid_t pid;
    int status;

    /* clean previous state */
    unlink(TPM_SOCK);
    unlink(CTRL_SOCK);
    mkdir(TPM_DIR, 0700);

    pid = launch_swtpm();
    if (pid < 0) {
        fprintf(stderr, "Failed to launch swtpm\n");
        return 1;
    }

    sleep(1);
    talk_to_tpm();

    printf("[mini-qemu] shutting down swtpm...\n");
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);

    printf("[mini-qemu] done.\n");
    return 0;
}

