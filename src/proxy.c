#include "proxy.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int proxy_start_local(const char *listen_host, int listen_port,
                      const char *forward_host, int forward_port) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        char l[32], f[32];
        snprintf(l, sizeof(l), "%s:%d", listen_host, listen_port);
        snprintf(f, sizeof(f), "%s:%d", forward_host, forward_port);

        // Requires udp-over-tcp installed: https://github.com/mullvad/udp-over-tcp
        // There are no binaries so cargo and rustup will have to be the latest version
        // to build and install tcp2udp.
        execlp("tcp2udp", "tcp2udp",
               "--tcp-listen", l,
               "--udp-forward", f,
               (char *)NULL);
        _exit(127);
    }
    return (int)pid;
}

void proxy_stop(int pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

int proxy_start_ssh_tunnel(const char *remote_user,
                           const char *remote_host,
                           int local_port,
                           int remote_port) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        char forward[128];
        snprintf(forward, sizeof(forward), "%d:localhost:%d",
                 local_port, remote_port);
        char target[256];
        snprintf(target, sizeof(target), "%s@%s",
                 remote_user, remote_host);

        execlp("ssh", "ssh", "-N", "-L", forward, target, (char *)NULL);
        _exit(127);
    }
    return (int)pid;
}

