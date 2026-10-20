#include "transport_internal.h"

#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void receiver_close_file(Transport *t)
{
  if (t->recv_file)
  {
    fclose(t->recv_file); t->recv_file = NULL;
  }

  t->recv_in_progress  = false;
  t->recv_expected_seq = 0;
  t->recv_highest_seq  = 0;

  if (t->recv_window)
  {
    for (int i = 0; i < REASSEMBLY_WINDOW; i++)
    {
      t->recv_window[i].filled = false;
    }
  }
}

bool receiver_open_file(Transport *t, const char *name, uint64_t file_size)
{
    receiver_close_file(t);

    if (!t->recv_window)
    {
      t->recv_window = calloc(REASSEMBLY_WINDOW, sizeof(IncomingChunk));

      if (!t->recv_window)
      {
        set_last_error(t, "out of memory for reassembly window");

        return false;
      }
    }

    snprintf(t->recv_filename, sizeof(t->recv_filename), "%s", name);
    t->recv_file = fopen(name, "wb");

    if (!t->recv_file)
    {
      set_last_error(t, "cannot open output file");

      return false;
    }

    t->recv_file_size     = file_size;
    t->recv_total_packets = (uint32_t)((file_size + WIRE_MAX_PAYLOAD - 1)
                                       / WIRE_MAX_PAYLOAD);
    t->recv_expected_seq  = 0;
    t->recv_highest_seq   = 0;
    t->recv_in_progress   = true;

    return true;
}

void receiver_send_ack(Transport *t, uint32_t ack, uint32_t ack_bits)
{
  if (!t->peer_known)
    return;

  uint8_t buf[WIRE_HEADER_SIZE];
  WireHeader h;

  memset(&h, 0, sizeof(h));
  h.magic        = WIRE_MAGIC;
  h.version      = WIRE_VERSION;
  h.type         = WIRE_ACK;
  h.conn_id      = t->conn_id;
  h.ack          = ack;
  h.ack_bits     = ack_bits;
  h.timestamp_us = now_us();
  h.payload_len  = 0;
  memcpy(buf, &h, sizeof(h));

  sendto(t->sock, buf, sizeof(h), 0,
        (struct sockaddr *)&t->peer_addr, sizeof(t->peer_addr));
}

static void receiver_flush_in_order(Transport *t)
{
  while (t->recv_in_progress)
  {
    uint32_t idx = t->recv_expected_seq % REASSEMBLY_WINDOW;
    IncomingChunk *c = &t->recv_window[idx];

    if (!c->filled || c->seq != t->recv_expected_seq)
      break;

    fwrite(c->data, 1, c->payload_len, t->recv_file);
    c->filled = false;
    t->recv_expected_seq++;
  }

  if (t->recv_total_packets > 0 &&
      t->recv_expected_seq >= t->recv_total_packets)
  {
    fclose(t->recv_file);
    t->recv_file = NULL;
    t->recv_in_progress = false;

    if (t->cbs.on_file_received)
        t->cbs.on_file_received(t->recv_filename, t->recv_file_size, t->user);
  }
}

void receiver_handle_data(Transport *t, const WireHeader *h,
                          const uint8_t *payload, size_t payload_len)
{
  if (!t->recv_in_progress)
    return;

  if (h->seq < t->recv_expected_seq)
    return;

  uint32_t idx = h->seq % REASSEMBLY_WINDOW;
  IncomingChunk *c = &t->recv_window[idx];

  if (c->filled && c->seq != h->seq)
    return;

  c->seq         = h->seq;
  c->payload_len = (uint32_t)payload_len;
  memcpy(c->data, payload, payload_len);
  c->filled = true;

  if (h->seq > t->recv_highest_seq)
    t->recv_highest_seq = h->seq;

  receiver_flush_in_order(t);
  uint32_t ack_seq = (t->recv_expected_seq == 0) ? 0 : t->recv_expected_seq - 1;

  receiver_send_ack(t, ack_seq, 0);
}

