#include "bbr.h"
#include "transport_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* BBR Constants */
#define BBR_UNIT 256
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)
#define BBR_MIN_RTT_WIN_SEC 10
#define BBR_MIN_RTT_WIN_US (BBR_MIN_RTT_WIN_SEC * 1000000ULL)

/* BBR gains, in BBR_UNIT fixed point */
#define BBR_STARTUP_GAIN ((uint32_t)(2.885 * BBR_UNIT))
#define BBR_DRAIN_GAIN ((uint32_t)(0.346 * BBR_UNIT))
#define BBR_CWND_GAIN (2 * BBR_UNIT)
#define BBR_MIN_CWND 4
#define BBR_BW_WINDOW_SLOTS 10

/* PROBE_BW gain cycle */
static const uint32_t BBR_PACING_GAIN[8] = {
    (uint32_t)(1.25 * BBR_UNIT),
    (uint32_t)(0.75 * BBR_UNIT),
    BBR_UNIT,
    BBR_UNIT,
    BBR_UNIT,
    BBR_UNIT,
    BBR_UNIT,
    BBR_UNIT,
};

struct Bbr {
  /* Mode & State */
  BbrMode mode;

  /* Bandwidth Estimation */
  uint64_t btl_bw;
  uint64_t bw_hi[BBR_BW_WINDOW_SLOTS];
  uint32_t bw_hi_idx;

  /* Round tracking */
  uint64_t next_rtt_delivered; /* cumulative delivered at last round start */
  uint32_t round_count;        /* packet-timed round counter */

  /* STARTUP */
  uint64_t full_bw;
  uint32_t full_bw_count;

  /* RTT Estimation */
  uint64_t min_rtt_us;
  uint64_t min_rtt_stamp_us;

  /* PROBE_BW */
  uint32_t cycle_idx;

  /* PROBE_RTT */
  uint64_t probe_rtt_done_us;

  /* Control */
  uint32_t pacing_gain;
  uint32_t cwnd_gain;
  uint64_t pacing_rate;
  uint32_t cwnd;

  uint32_t init_cwnd;
};

Bbr *bbr_create(uint32_t initial_cwnd) {
  Bbr *b = calloc(1, sizeof(Bbr));

  if (!b)
    return NULL;

  b->mode = BBR_STARTUP;
  b->min_rtt_us = ~0ULL;
  b->min_rtt_stamp_us = now_us();
  b->pacing_gain = BBR_STARTUP_GAIN;
  b->cwnd_gain = BBR_CWND_GAIN;
  b->pacing_rate = 1 * 1024 * 1024;
  b->cwnd = initial_cwnd;
  b->init_cwnd = initial_cwnd;
  return b;
}

void bbr_destroy(Bbr *b) {
  if (b)
    free(b);
}

BbrDecision bbr_on_init(Bbr *b) {
  if (!b)
    return (BbrDecision){0};

  BbrDecision d = {b->pacing_rate, b->cwnd, b->mode};

  return d;
}

static uint64_t bdp_bytes(const Bbr *b) {
  if (b->min_rtt_us == ~0ULL || b->btl_bw == 0)
    return 0;

  return (b->btl_bw * b->min_rtt_us) / 1000000ULL;
}

static void bbr_enter_probe_bw(Bbr *b) {
  b->mode = BBR_PROBE_BW;
  b->cycle_idx = 2;
  b->pacing_gain = BBR_PACING_GAIN[b->cycle_idx];
  b->cwnd_gain = BBR_CWND_GAIN;
}

BbrDecision bbr_on_ack(Bbr *b, const RateSample *rs, uint32_t packets_in_flight,
                       uint32_t bytes_in_flight) {
  if (!b || !rs)
    return (BbrDecision){0};

  (void)bytes_in_flight;

  uint64_t now = now_us();

  /* --- RTT model --- */
  if (rs->rtt_us > 0) {
    if ((uint64_t)rs->rtt_us < b->min_rtt_us ||
        now - b->min_rtt_stamp_us > BBR_MIN_RTT_WIN_US) {
      b->min_rtt_us = (uint64_t)rs->rtt_us;
      b->min_rtt_stamp_us = now;
    }
  }

  /* --- Bandwidth model: record the sample for this round --- */
  if (rs->interval_us > 0 && rs->delivered > rs->prior_delivered) {
    uint64_t delivered_bytes =
        (rs->delivered - rs->prior_delivered) * WIRE_MAX_PAYLOAD;
    uint64_t bw_sample = (delivered_bytes * 1000000ULL) / rs->interval_us;

    /* Keep the running max within the current round's slot */
    if (bw_sample > b->bw_hi[b->bw_hi_idx])
      b->bw_hi[b->bw_hi_idx] = bw_sample;
  }

  /* --- Round tracking: packet-timed round trip --- */
  if (rs->prior_delivered >= b->next_rtt_delivered) {
    b->next_rtt_delivered = rs->delivered;
    b->round_count++;

    /* Advance to the next window slot (ring of BBR_BW_WINDOW_SLOTS) */
    b->bw_hi_idx = (b->bw_hi_idx + 1) % BBR_BW_WINDOW_SLOTS;
    b->bw_hi[b->bw_hi_idx] = 0;

    /* btl_bw = max over the last N rounds */
    uint64_t max_bw = 0;
    for (int i = 0; i < BBR_BW_WINDOW_SLOTS; i++) {
      if (b->bw_hi[i] > max_bw)
        max_bw = b->bw_hi[i];
    }
    b->btl_bw = max_bw;

    /* Safety floor to prevent pacer lockup */
    if (b->btl_bw < 1000)
      b->btl_bw = 1000;

    fprintf(stderr, "BBR_MODEL: round=%u min_rtt=%llu us btl_bw=%llu bytes/s\n",
            b->round_count, (unsigned long long)b->min_rtt_us,
            (unsigned long long)b->btl_bw);

    /* --- STARTUP exit condition --- */
    if (b->mode == BBR_STARTUP) {
      if (b->btl_bw >= (b->full_bw * 5) / 4) {
        b->full_bw = b->btl_bw;
        b->full_bw_count = 0;
      } else {
        b->full_bw_count++;
        if (b->full_bw_count >= 3) {
          fprintf(stderr, "BBR: STARTUP -> DRAIN (btl_bw=%llu)\n",
                  (unsigned long long)b->btl_bw);
          b->mode = BBR_DRAIN;
          b->pacing_gain = BBR_DRAIN_GAIN;
        }
      }
    } else if (b->mode == BBR_PROBE_BW) {
      b->cycle_idx = (b->cycle_idx + 1) % 8;
      b->pacing_gain = BBR_PACING_GAIN[b->cycle_idx];
    }
  }

  /* --- DRAIN exit condition --- */
  if (b->mode == BBR_DRAIN) {
    uint64_t bdp = bdp_bytes(b);
    uint64_t inflight_bytes = packets_in_flight * WIRE_MAX_PAYLOAD;

    if (inflight_bytes <= bdp + WIRE_MAX_PAYLOAD) {
      fprintf(stderr, "BBR: DRAIN -> PROBE_BW (inflight=%llu bdp=%llu)\n",
              (unsigned long long)inflight_bytes, (unsigned long long)bdp);
      bbr_enter_probe_bw(b);
    }
  }

  /* --- PROBE_RTT entry --- */
  if (b->mode != BBR_PROBE_RTT &&
      now - b->min_rtt_stamp_us > BBR_MIN_RTT_WIN_US) {
    fprintf(stderr, "BBR: -> PROBE_RTT\n");
    b->mode = BBR_PROBE_RTT;
    b->pacing_gain = BBR_UNIT;
    b->cwnd_gain = BBR_UNIT;
    b->probe_rtt_done_us = now + 200000;
  }

  /* --- PROBE_RTT exit --- */
  if (b->mode == BBR_PROBE_RTT && now >= b->probe_rtt_done_us) {
    fprintf(stderr, "BBR: PROBE_RTT -> PROBE_BW\n");
    b->min_rtt_stamp_us = now;
    bbr_enter_probe_bw(b);
  }

  /* --- Control: pacing rate and cwnd --- */
  if (b->btl_bw > 0) {
    b->pacing_rate = (b->btl_bw * b->pacing_gain) / BBR_UNIT;
  }
  if (b->pacing_rate < 1000)
    b->pacing_rate = 1000;

  /* BBR cwnd logic */
  if (b->mode == BBR_STARTUP) {
    fprintf(stderr, "BBR_CWND: startup, before=%u acked_sacked=%u\n", b->cwnd,
            rs->acked_sacked);
    b->cwnd += rs->acked_sacked;
    fprintf(stderr, "BBR_CWND: startup, after=%u\n", b->cwnd);
  } else {
    uint64_t bdp = bdp_bytes(b);
    uint32_t target_cwnd =
        (uint32_t)((bdp * b->cwnd_gain) / (WIRE_MAX_PAYLOAD * BBR_UNIT));
    if (target_cwnd < b->init_cwnd)
      target_cwnd = b->init_cwnd;
    fprintf(stderr, "BBR_CWND: steady, target=%u bdp=%llu\n", target_cwnd,
            (unsigned long long)bdp);
    b->cwnd = target_cwnd;
  }

  if (b->mode == BBR_DRAIN) {
    uint64_t bdp = bdp_bytes(b);
    uint64_t inflight_bytes = packets_in_flight * WIRE_MAX_PAYLOAD;
    if (inflight_bytes <= bdp + WIRE_MAX_PAYLOAD) {
      fprintf(stderr, "BBR: DRAIN -> PROBE_BW (inflight=%llu bdp=%llu)\n",
              (unsigned long long)inflight_bytes, (unsigned long long)bdp);
      bbr_enter_probe_bw(b);
    }
  }

  if (b->mode == BBR_PROBE_RTT) {
    b->cwnd = BBR_MIN_CWND;
  }

  uint64_t bdp = bdp_bytes(b);
  uint32_t target_cwnd =
      (uint32_t)((bdp * b->cwnd_gain) / (WIRE_MAX_PAYLOAD * BBR_UNIT));

  if (target_cwnd < b->init_cwnd)
    target_cwnd = b->init_cwnd;

  if (b->mode == BBR_PROBE_RTT)
    target_cwnd = BBR_MIN_CWND;

  b->cwnd = target_cwnd;

  fprintf(stderr, "BBR_RETURN: cwnd=%u mode=%d\n", b->cwnd, b->mode);
  BbrDecision d = {b->pacing_rate, b->cwnd, b->mode};

  return d;
}

void bbr_on_loss(Bbr *b) { (void)b; }
BbrMode bbr_mode(const Bbr *b) { return b ? b->mode : BBR_STARTUP; }
uint64_t bbr_pacing_rate(const Bbr *b) { return b ? b->pacing_rate : 0; }
uint32_t bbr_cwnd(const Bbr *b) { return b ? b->cwnd : 0; }
uint64_t bbr_min_rtt_us(const Bbr *b) { return b ? b->min_rtt_us : 0; }
uint64_t bbr_btl_bw(const Bbr *b) { return b ? b->btl_bw : 0; }
