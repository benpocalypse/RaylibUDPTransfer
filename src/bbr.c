#include "bbr.h"
#include <stdlib.h>

/* This is just the temporary stub implementation I'm working on. */

struct Bbr { int placeholder; };

Bbr *bbr_create(uint32_t initial_cwnd) {
    (void)initial_cwnd;
    return calloc(1, sizeof(Bbr));
}

void bbr_destroy(Bbr *b) { free(b); }

BbrDecision bbr_on_init(Bbr *b) {
    (void)b;
    BbrDecision d = { .pacing_rate = 1u << 20, .cwnd = 10, .mode = BBR_STARTUP };
    return d;
}

BbrDecision bbr_on_ack(Bbr *b, const RateSample *rs,
                       uint32_t pif, uint32_t bif) {
    (void)b; (void)rs; (void)pif; (void)bif;
    BbrDecision d = { .pacing_rate = 1u << 20, .cwnd = 10, .mode = BBR_STARTUP };
    return d;
}

void bbr_on_loss(Bbr *b) { (void)b; }
BbrMode bbr_mode(const Bbr *b) { (void)b; return BBR_STARTUP; }
uint64_t bbr_pacing_rate(const Bbr *b) { (void)b; return 0; }
uint32_t bbr_cwnd(const Bbr *b) { (void)b; return 0; }
uint64_t bbr_min_rtt_us(const Bbr *b) { (void)b; return 0; }
uint64_t bbr_btl_bw(const Bbr *b) { (void)b; return 0; }
                                                       
