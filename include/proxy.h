#ifndef PROXY_H
#define PROXY_H

#include <stdbool.h>

// Spawns the local udp-over-tcp proxy as a child process.
// Returns the child PID on success, -1 on failure.
int proxy_start_local(const char *listen_host, int listen_port,
                      const char *forward_host, int forward_port);

// Kills the proxy child process.
void proxy_stop(int pid);

// Builds and runs the SSH tunnel command:
//   ssh -N -L <local>:localhost:<remote> user@host
int proxy_start_ssh_tunnel(const char *remote_user,
                           const char *remote_host,
                           int local_port,
                           int remote_port);

#endif

