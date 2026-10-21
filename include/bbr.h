#ifndef BBR_H
#define BBR_H

#include "ratesample.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct Bbr Bbr;

typedef enum { BBR_STARTUP, BBR_DRAIN, BBR_PROBE_BW, BBR_PROBE_RTT } BbrMode;

/*
 * Create a BBR controller. The transport owns it. BBR owns no sockets.
 * initial_cwnd is in packets. typical value is 10.
 */
Bbr *bbr_create(uint32_t initial_cwnd);
void bbr_destroy(Bbr *b);

/*
 * Called by the transport once per ACK, with a fully-populated
 * RateSample. BBR updates its internal model and returns the
 * control decisions the transport must apply.
 */
typedef struct {
  uint64_t pacing_rate; /* bytes per second */
  uint32_t cwnd;        /* packets */
  BbrMode mode;         /* current state, for diagnostics */
} BbrDecision;

BbrDecision bbr_on_ack(Bbr *b, const RateSample *rs, uint32_t packets_in_flight,
                       uint32_t bytes_in_flight);

/*
 * Called by the transport on connection start, before any ACK arrives.
 * Returns the initial pacing rate and cwnd.
 */
BbrDecision bbr_on_init(Bbr *b);

/*
 * Called by the transport when a loss is detected outside of an ACK
 * (e.g. RTO expiry).
 */
void bbr_on_loss(Bbr *b);

/* Diagnostics */
BbrMode bbr_mode(const Bbr *b);
uint64_t bbr_pacing_rate(const Bbr *b);
uint32_t bbr_cwnd(const Bbr *b);
uint64_t bbr_min_rtt_us(const Bbr *b);
uint64_t bbr_btl_bw(const Bbr *b);

#endif

