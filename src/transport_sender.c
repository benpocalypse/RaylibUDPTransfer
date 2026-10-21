#include "transport_internal.h"

#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ---- helpers ---- */

static void sender_remember_packet(Transport *t, uint32_t seq,
                                   uint64_t send_time_us,
                                   uint32_t payload_len,
                                   const uint8_t *buf, size_t buf_len,
                                   uint64_t delivered_at_send,
                                   uint32_t in_flight_at_send)
{
  OutgoingPacket *op = &t->outgoing[seq % OUTGOING_QUEUE_SIZE];
  op->seq               = seq;
  op->send_time_us      = send_time_us;
  op->payload_len       = payload_len;
  op->in_use            = true;
  op->delivered_at_send = delivered_at_send;
  op->in_flight_at_send = in_flight_at_send;

  if (buf_len <= sizeof(op->buf))
    memcpy(op->buf, buf, buf_len);
}

/* ---- public-internal ---- */

void sender_close_file(Transport *t)
{
    if (t->send_file)
    {
      fclose(t->send_file);
      t->send_file = NULL;
    }
}

bool sender_open_file(Transport *t)
{
  FILE *f = fopen(t->send_path, "rb");
  t->send_completed_ok = false;

  if (!f)
  {
    set_last_error(t, "cannot open file for sending");

    return false;
  }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (sz < 0)
    {
      fclose(f);
      set_last_error(t, "cannot determine file size");

      return false;
    }

  t->send_file          = f;
  t->send_file_size     = (uint64_t)sz;
  t->send_total_packets = (uint32_t)((t->send_file_size + WIRE_MAX_PAYLOAD - 1)
                                     / WIRE_MAX_PAYLOAD);
  t->send_next_seq      = 0;
  t->send_acked_seq     = 0;
  t->send_in_progress   = true;

  t->next_send_time_us  = 0;

  uint8_t ctrl_buf[WIRE_HEADER_SIZE + sizeof(ControlHeader) + 256];
  WireHeader ch;

  memset(&ch, 0, sizeof(ch));
  ch.magic        = WIRE_MAGIC;
  ch.version      = WIRE_VERSION;
  ch.type         = WIRE_CONTROL;
  ch.conn_id      = t->conn_id;
  ch.timestamp_us = now_us();

  const char *base = strrchr(t->send_path, '/');
  base = base ? base + 1 : t->send_path;

  ControlHeader ctl;
  memset(&ctl, 0, sizeof(ctl));
  ctl.control_type  = CONTROL_FILE_HEADER;
  ctl.name_len      = (uint32_t)strlen(base);
  ctl.file_size     = t->send_file_size;
  ctl.total_packets = t->send_total_packets;

  ch.payload_len = (uint32_t)(sizeof(ctl) + ctl.name_len);

  memcpy(ctrl_buf, &ch, sizeof(ch));
  memcpy(ctrl_buf + sizeof(ch), &ctl, sizeof(ctl));
  memcpy(ctrl_buf + sizeof(ch) + sizeof(ctl), base, ctl.name_len);

  sendto(t->sock, ctrl_buf, sizeof(ch) + sizeof(ctl) + ctl.name_len, 0,
         (struct sockaddr *)&t->peer_addr, sizeof(t->peer_addr));

  fprintf(stderr, "SENDER: open '%s' size=%llu packets=%u\n",
          base, (unsigned long long)t->send_file_size,
          t->send_total_packets);

  return true;
}

void sender_pump(Transport *t)
{
  if (!t->send_in_progress || !t->send_file || !t->peer_known)
    return;

  /* Refill pacing tokens based on elapsed time */
  uint64_t now = now_us();
  uint64_t elapsed = now - t->last_poll_us;

  if (elapsed > 0 && t->pacing_rate > 0)
  {
    uint64_t add = (t->pacing_rate * elapsed) / 1000000ULL;
    t->pacing_tokens += add;
    t->last_poll_us = now;

    if (t->pacing_tokens > t->pacing_rate)
      t->pacing_tokens = t->pacing_rate;
  }

  uint8_t packet[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];

  for (;;)
  {
    if (t->send_next_seq >= t->send_total_packets)
    {
      sender_close_file(t);
      return;
    }

    /* Use BBR's cwnd instead of the hardcoded ring size */
    uint32_t cwnd = bbr_cwnd(t->bbr);
    if (cwnd > OUTGOING_QUEUE_SIZE)
      cwnd = OUTGOING_QUEUE_SIZE; /* Safety cap */

    if (t->send_next_seq >= t->send_acked_seq + cwnd)
    {
      return;
    }

    /* Pacing gate: wait for tokens */
    if (t->pacing_tokens < WIRE_MAX_PAYLOAD) return;
    t->pacing_tokens -= WIRE_MAX_PAYLOAD;

    /* Stage 2e: pacing gate */
    uint64_t now = now_us();

    if (now < t->next_send_time_us)
      return;

    uint8_t payload[WIRE_MAX_PAYLOAD];
    size_t n = fread(payload, 1, sizeof(payload), t->send_file);

    if (n == 0)
    {
      sender_close_file(t);
      return;
    }

    size_t pkt_len = build_data_packet(t, t->send_next_seq,
                                       payload, (uint32_t)n,
                                       packet, sizeof(packet));

    if (pkt_len == 0)
    {
      set_last_error(t, "packet build failed");
      sender_close_file(t);
      return;
    }

    ssize_t rc = sendto(t->sock, packet, pkt_len, 0,
                        (struct sockaddr *)&t->peer_addr,
                        sizeof(t->peer_addr));

    if (rc < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;

      set_last_error(t, "sendto failed");
      sender_close_file(t);

      return;
    }

    uint32_t in_flight = t->send_next_seq - t->send_acked_seq;
    uint64_t delivered = (uint64_t)t->send_acked_seq * WIRE_MAX_PAYLOAD;

    sender_remember_packet(t, t->send_next_seq, now, (uint32_t)n,
                           packet, pkt_len, delivered, in_flight);

    t->send_next_seq++;

    /* Pacing: schedule next transmit time */
    uint64_t wait_us = ((uint64_t)n * 1000000ULL) / (t->pacing_rate ? t->pacing_rate : 1);
    t->next_send_time_us = now + wait_us;

    if (t->cbs.on_progress && t->send_total_packets > 0)
    {
      float p = (float)t->send_next_seq / (float)t->send_total_packets;
      t->cbs.on_progress(p, t->user);
    }
  }
}

void sender_check_timeouts(Transport *t)
{
  if (!t->send_in_progress || !t->peer_known || t->send_completed_ok)
    return;

  uint64_t now = now_us();

  for (int i = 0; i < OUTGOING_QUEUE_SIZE; i++)
  {
    OutgoingPacket *op = &t->outgoing[i];

    if (!op->in_use)
      continue;

    if (op->seq >= t->send_total_packets)
      continue;

    if (now - op->send_time_us < RTO_US)
      continue;

    size_t pkt_len = WIRE_HEADER_SIZE + op->payload_len;
    ssize_t rc = sendto(t->sock, op->buf, pkt_len, 0,
                        (struct sockaddr *)&t->peer_addr,
                        sizeof(t->peer_addr));

    if (rc < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;

        continue;
    }

    op->send_time_us = now;
    fprintf(stderr, "SENDER: retransmit seq=%u\n", op->seq);
  }
}

void sender_handle_ack(Transport *t, const WireHeader *h)
{
  uint32_t base = h->ack;
  uint32_t bits = h->ack_bits;

  /* Stage 2d: build a RateSample from the packet metadata */
  OutgoingPacket *base_op = &t->outgoing[base % OUTGOING_QUEUE_SIZE];

  if (base_op->in_use && base_op->seq == base)
  {
    RateSample rs;

    memset(&rs, 0, sizeof(rs));
    /* BBR expects delivered to be the number of packets delivered
     * in this specific sample interval, not cumulative. */
    uint64_t current_delivered = t->send_acked_seq + 1;
    rs.interval_us     = now_us() - base_op->send_time_us;
    rs.rtt_us          = (int64_t)rs.interval_us;
    rs.delivered = t->send_acked_seq + 1;
    rs.prior_delivered = base_op->delivered_at_send / WIRE_MAX_PAYLOAD;
    fprintf(stderr, "SENDER: rs.acked_sacked=%u base=%u delivered=%llu prior_delivered=%llu\n",
            rs.acked_sacked, base, (unsigned long long)rs.delivered,
            (unsigned long long)rs.prior_delivered);
    rs.acked_sacked    = 1;
    fprintf(stderr, "SENDER: rs.acked_sacked=%u (base=%u)\n", rs.acked_sacked, base);
    rs.prior_in_flight = base_op->in_flight_at_send;
    rs.tx_in_flight    = base_op->in_flight_at_send;

    /* Stage 2f: Pass the sample to BBR */
    BbrDecision dec = bbr_on_ack(t->bbr, &rs,
                                 t->send_next_seq - t->send_acked_seq,
                                 (t->send_next_seq - t->send_acked_seq) * WIRE_MAX_PAYLOAD);

    /* Apply BBR's pacing rate decision to the transport */
    t->pacing_rate = dec.pacing_rate;

    fprintf(stderr, "BBR_DECISION: mode=%d pacing_rate=%llu cwnd=%u\n",
            dec.mode, (unsigned long long)dec.pacing_rate, dec.cwnd);

    fprintf(stderr, "BBR_INPUT: rtt=%llu us inflight=%u\n",
            (unsigned long long)rs.interval_us, rs.tx_in_flight);
  }

  /* Clear base */
  if (base_op->in_use && base_op->seq == base)
    base_op->in_use = false;

  /* Clear SACKed */
  for (int i = 0; i < 32; i++)
  {
    if (!(bits & (1u << i)))
      continue;

    uint32_t s = base - 1 - (uint32_t)i;

    if (s > base)
      continue;

    OutgoingPacket *sop = &t->outgoing[s % OUTGOING_QUEUE_SIZE];

    if (sop->in_use && sop->seq == s)
      sop->in_use = false;
  }

  /* Advance contiguous */
  while (t->send_acked_seq < t->send_total_packets)
  {
    OutgoingPacket *c = &t->outgoing[t->send_acked_seq % OUTGOING_QUEUE_SIZE];

    if (c->seq != t->send_acked_seq)
      break;

    if (c->in_use)
      break;

    t->send_acked_seq++;
  }

  if (t->send_acked_seq >= t->send_total_packets &&
      t->send_total_packets > 0)
  {
    fprintf(stderr, "SENDER: transfer complete (acked=%u/%u)\n",
            t->send_acked_seq, t->send_total_packets);
    t->send_completed_ok = true;
    t->send_in_progress  = false;

    if (t->cbs.on_progress)
      t->cbs.on_progress(1.0f, t->user);
  }
}

