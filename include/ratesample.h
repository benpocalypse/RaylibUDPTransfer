#ifndef RATESAMPLE_H
#define RATESAMPLE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * One RateSample is produced by the transport every time a DATA
 * packet is acknowledged. BBR consumes it via bbr_on_ack().
 *
 * Fields are named to match the IETF BBR draft and the Linux
 * rate_sample struct, so the algorithm port is mechanical.
 */

typedef struct {
    /* How much new data did this ACK cover? */
    uint64_t delivered;        /* cumulative packets delivered */
    uint64_t prior_delivered;  /* delivered when the ACKed packet was sent */
    uint32_t acked_sacked;     /* packets newly ACKed by this ACK */

    /* Timing */
    uint64_t interval_us;      /* send-to-ack time for the ACKed packet */
    int64_t  rtt_us;           /* latest RTT sample; -1 if none */

    /* In-flight snapshots */
    uint32_t prior_in_flight;  /* in-flight when round started */
    uint32_t tx_in_flight;     /* in-flight when the ACKed packet was sent */

    /* Loss / ECN */
    uint32_t losses;           /* loss events in this sample */
    int32_t  lost;             /* packets declared lost */
    uint32_t delivered_ce;     /* ECN-CE-marked deliveries */

    /* Flags */
    bool is_app_limited;       /* was the sender idle before sending? */
    bool is_ack_delayed;       /* did the receiver delay this ACK? */
    bool is_acking_tlp_retrans_seq; /* ACKing a TLP retransmit */
} RateSample;

#endif

