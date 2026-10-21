#ifndef TRANSPORT_INTERNAL_H
#define TRANSPORT_INTERNAL_H

#include "transport.h"
#include "bbr.h"
#include "wire.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <netinet/in.h>

#define MAX_PATH_LEN_LOCAL 512
#define REASSEMBLY_WINDOW  4096
#define OUTGOING_QUEUE_SIZE 256
#define RTO_US (500 * 1000)

typedef struct
{
  uint32_t seq;
  uint64_t send_time_us;
  uint32_t payload_len;
  uint8_t  buf[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];
  bool     in_use;

  /* Stage 2d: snapshots taken at send time */
  uint64_t delivered_at_send;
  uint32_t in_flight_at_send;
} OutgoingPacket;

typedef struct
{
  uint32_t seq;
  uint32_t payload_len;
  uint8_t  data[WIRE_MAX_PAYLOAD];
  bool     filled;
} IncomingChunk;

struct Transport
{
  /* Socket */
  int sock;
  struct sockaddr_in bind_addr;
  struct sockaddr_in peer_addr;
  bool peer_known;

  /* Callbacks */
  TransportCallbacks cbs;
  void *user;

  /* Diagnostics */
  char last_error[256];

  /* Sender state */
  FILE *send_file;
  uint64_t send_file_size;
  uint32_t send_total_packets;
  uint32_t send_next_seq;
  uint32_t send_acked_seq;
  bool     send_completed_ok;
  bool     send_in_progress;
  char     send_path[MAX_PATH_LEN_LOCAL];
  bool     has_queued_send;

  /* Pacing (Stage 2e) */
  uint64_t pacing_rate;
  uint64_t next_send_time_us;
  uint64_t pacing_tokens;    /* bytes allowed to send right now */
  uint64_t last_poll_us;     /* when tokens were last refilled */

  /* Receiver state */
  FILE *recv_file;
  char recv_filename[MAX_PATH_LEN_LOCAL];
  uint64_t recv_file_size;
  uint32_t recv_total_packets;
  uint32_t recv_expected_seq;
  uint32_t recv_highest_seq;
  bool     recv_in_progress;
  IncomingChunk *recv_window;

  /* Connection identity */
  uint32_t conn_id;
  uint32_t sender_conn_id;

  /* Actual BBR implementation */
  Bbr *bbr;

  /* Bookkeeping */
  OutgoingPacket *outgoing;
  bool cancelled;
};

/* Shared helpers, implemented in transport_utils.c */
uint64_t now_us(void);
void     set_last_error(Transport *t, const char *msg);
size_t   build_data_packet(Transport *t, uint32_t seq,
                           const uint8_t *payload, uint32_t payload_len,
                           uint8_t *out, size_t out_cap);
bool     parse_header(const uint8_t *buf, size_t len, WireHeader *out);

/* Sender, implemented in transport_sender.c */
void sender_open_file_or_report(Transport *t);
bool sender_open_file(Transport *t);
void sender_close_file(Transport *t);
void sender_pump(Transport *t);
void sender_check_timeouts(Transport *t);
void sender_handle_ack(Transport *t, const WireHeader *h);

/* Receiver, implemented in transport_receiver.c */
void receiver_open_file_or_report(Transport *t,
                                  const char *name, uint64_t file_size);
bool receiver_open_file(Transport *t, const char *name, uint64_t file_size);
void receiver_close_file(Transport *t);
void receiver_handle_data(Transport *t, const WireHeader *h,
                          const uint8_t *payload, size_t payload_len);
void receiver_send_ack(Transport *t, uint32_t ack, uint32_t ack_bits);

/* Test helper to create a transport without opening a real socket */
Transport *transport_create_for_test(void);

#endif

