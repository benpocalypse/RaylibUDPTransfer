#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include "ratesample.h"

typedef struct Transport Transport;

typedef struct
{
    void (*on_progress)(float progress, void *user);
    void (*on_file_received)(const char *name, uint64_t size, void *user);
    void (*on_error)(const char *message, void *user);
} TransportCallbacks;

/*
 * Lifecycle functions
 */
Transport *transport_create(const char *bind_host, int bind_port,
                            const char *peer_host, int peer_port,
                            const TransportCallbacks *cbs, void *user);
void transport_destroy(Transport *t);

/*
 * Sending. queue_send copies the path and starts the transfer
 * asynchronously. poll() must be called regularly from a single
 * thread; it is the only function that advances time, sends
 * packets, processes ACKs, runs BBR, and detects losses.
 */
bool transport_queue_send(Transport *t, const char *path);
void transport_poll(Transport *t);
void transport_cancel(Transport *t);
bool transport_send_complete(const Transport *t);

/*
 * Diagnostics (for the GUI)
 */
float transport_get_progress(const Transport *t);
uint64_t transport_get_pacing_rate(const Transport *t);
uint64_t transport_get_cwnd(const Transport *t);
uint64_t transport_get_min_rtt_us(const Transport *t);
const char *transport_get_bbr_state(const Transport *t);
const char *transport_get_last_error(const Transport *t);

#endif

