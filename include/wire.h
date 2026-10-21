#ifndef WIRE_H
#define WIRE_H

#include <stdint.h>

/*
 * Wire protocol for the reliable UDP transport.
 *
 * Every packet begins with a WireHeader. DATA packets carry a payload
 * of up to WIRE_MAX_PAYLOAD bytes. ACK and CONTROL packets carry
 * no payload.
 *
 * All multi-byte fields are in network byte order (big-endian).
 * Timestamps are in microseconds since an arbitrary epoch chosen
 * by the sender at connection start (monotonic clock).
 */

#define WIRE_MAGIC 0xB8B8B8B8u
#define WIRE_VERSION 1

/* conservative MTU safety margin
* This value was chosen to work safely with
* most home routers and internet connections.
*/
#define WIRE_MAX_PAYLOAD 1200
#define WIRE_HEADER_SIZE 40

typedef enum {
  WIRE_DATA = 1,   /* carries file payload */
  WIRE_ACK = 2,    /* acknowledges one or more DATA packets */
  WIRE_CONTROL = 3 /* file header, completion, cancel, etc. */
} WireType;

typedef struct __attribute__((packed)) {
  uint32_t magic;        /* WIRE_MAGIC, sanity check */
  uint8_t version;       /* WIRE_VERSION */
  uint8_t type;          /* WireType */
  uint16_t flags;        /* reserved, must be zero */
  uint32_t conn_id;      /* random per-connection identifier */
  uint64_t seq;          /* DATA: packet sequence; ACK: ack'd seq */
  uint32_t ack;          /* highest contiguous seq the sender has seen */
  uint32_t ack_bits;     /* bitmask of 32 seqs below "ack" that are ACKed */
  uint64_t timestamp_us; /* sender's send time, for RTT */
  uint32_t payload_len;  /* 0 for ACK/CONTROL, else bytes following */
} WireHeader;

/*
 * A CONTROL packet's payload begins with a ControlHeader followed by
 * type-specific bytes (filename, file size, etc.).
 */
#define CONTROL_FILE_HEADER 1
#define CONTROL_FILE_COMPLETE 2
#define CONTROL_FILE_ABORT 3

typedef struct __attribute__((packed)) {
  uint8_t control_type; /* CONTROL_* */
  uint8_t reserved[3];
  uint32_t name_len;      /* bytes of filename following this header */
  uint64_t file_size;     /* total bytes of file */
  uint32_t total_packets; /* total DATA packets the sender will send */
  uint32_t reserved2;
} ControlHeader;

#endif

