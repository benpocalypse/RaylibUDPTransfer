#include "transport.h"
#include "bbr.h"
#include <stdlib.h>

/* This is just the temporary stub implementation I'm working on. */

struct Transport { int placeholder; };

Transport *transport_create(const char *bh, int bp,
                            const char *ph, int pp,
                            const TransportCallbacks *cbs, void *user) {
    (void)bh; (void)bp; (void)ph; (void)pp; (void)cbs; (void)user;
    return calloc(1, sizeof(Transport));
}

void transport_destroy(Transport *t) { free(t); }
bool transport_queue_send(Transport *t, const char *path) { (void)t; (void)path; return false; }
void transport_poll(Transport *t) { (void)t; }
void transport_cancel(Transport *t) { (void)t; }
float transport_get_progress(const Transport *t) { (void)t; return 0.0f; }
uint64_t transport_get_pacing_rate(const Transport *t) { (void)t; return 0; }
uint64_t transport_get_cwnd(const Transport *t) { (void)t; return 0; }
uint64_t transport_get_min_rtt_us(const Transport *t) { (void)t; return 0; }
const char *transport_get_bbr_state(const Transport *t) { (void)t; return "STUB"; }
const char *transport_get_last_error(const Transport *t) { (void)t; return ""; }

