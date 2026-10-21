#include "transport_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void set_last_error(Transport *t, const char *msg) {
  snprintf(t->last_error, sizeof(t->last_error), "%s", msg);

  if (t->cbs.on_error)
    t->cbs.on_error(msg, t->user);
}

size_t build_data_packet(Transport *t, uint32_t seq, const uint8_t *payload,
                         uint32_t payload_len, uint8_t *out, size_t out_cap) {
  if (out_cap < WIRE_HEADER_SIZE + payload_len)
    return 0;

  WireHeader h;
  memset(&h, 0, sizeof(h));
  h.magic = WIRE_MAGIC;
  h.version = WIRE_VERSION;
  h.type = WIRE_DATA;
  h.flags = 0;
  h.conn_id = t->conn_id;
  h.seq = seq;
  h.ack = 0;
  h.ack_bits = 0;
  h.timestamp_us = now_us();
  h.payload_len = payload_len;

  memcpy(out, &h, sizeof(h));
  memcpy(out + sizeof(h), payload, payload_len);

  return sizeof(h) + payload_len;
}

bool parse_header(const uint8_t *buf, size_t len, WireHeader *out) {
  if (len < WIRE_HEADER_SIZE)
    return false;

  memcpy(out, buf, sizeof(*out));

  if (out->magic != WIRE_MAGIC)
    return false;

  if (out->version != WIRE_VERSION)
    return false;

  if (out->payload_len > WIRE_MAX_PAYLOAD)
    return false;

  if (WIRE_HEADER_SIZE + out->payload_len > len)
    return false;

  return true;
}
