#include "bbr.h"
#include "transport.h"
#include "wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH_LEN_LOCAL 512
#define REASSEMBLY_WINDOW 4096 /* max out-of-order packets buffered */

/* Per-packet metadata kept on the sender side. Stage 2d will expand
 * this with delivered_at_send / in_flight_at_send snapshots. */
typedef struct
{
  uint32_t seq;
  uint64_t send_time_us;
  uint32_t payload_len;
  uint8_t buf[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];
  bool in_use;
} OutgoingPacket;

/* A single reassembly slot on the receiver side. */
typedef struct
{
  uint32_t seq;
  uint32_t payload_len;
  uint8_t data[WIRE_MAX_PAYLOAD];
  bool filled;
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
  uint32_t send_acked_seq; /* reserved for Stage 2b */
  bool send_in_progress;
  char send_path[MAX_PATH_LEN_LOCAL];
  bool has_queued_send;

  /* Receiver state */
  FILE *recv_file;
  char recv_filename[MAX_PATH_LEN_LOCAL];
  uint64_t recv_file_size;
  uint32_t recv_total_packets;
  uint32_t recv_expected_seq; /* next in-order seq we want */
  uint32_t recv_highest_seq;  /* highest seq seen so far */
  bool recv_in_progress;
  IncomingChunk *recv_window; /* [REASSEMBLY_WINDOW] ring */

  /* Connection identity */
  uint32_t conn_id;
  uint32_t sender_conn_id; /* learned from first packet, 0 until known */

  /* Book keeping */
  bool cancelled;
};

/* ------------------------------------------------------------------ *
 *  Time helpers
 * ------------------------------------------------------------------ */

static uint64_t now_us (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}

/* ------------------------------------------------------------------ *
 *  Socket helpers
 * ------------------------------------------------------------------ */

static int make_udp_socket (const char *host, int port, struct sockaddr_in *out_addr)
{
  int fd = socket (AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;

  int flags = fcntl (fd, F_GETFL, 0);
  if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
  {
    close (fd);
    return -1;
  }

  int rcvbuf = 4 * 1024 * 1024;
  setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof (rcvbuf));

  memset (out_addr, 0, sizeof (*out_addr));
  out_addr->sin_family = AF_INET;
  out_addr->sin_port = htons ((uint16_t) port);

  if (inet_pton (AF_INET, host, &out_addr->sin_addr) != 1)
  {
    close (fd);
    return -1;
  }

  if (bind (fd, (struct sockaddr *) out_addr, sizeof (*out_addr)) < 0)
  {
    close (fd);
    return -1;
  }

  return fd;
}

static void set_last_error (Transport *t, const char *msg)
{
  snprintf (t->last_error, sizeof (t->last_error), "%s", msg);

  if (t->cbs.on_error)
    t->cbs.on_error (msg, t->user);
}

/* ------------------------------------------------------------------ *
 *  Wire serialization
 *
 *  The header is packed, but the fields are host-endian. We send them
 *  as-is for Stage 2a. Stage 2b will swap to network byte order via
 *  htonl/htons before we care about cross-architecture compatibility.
 *  For a same-architecture demo this is fine; document it and move on.
 * ------------------------------------------------------------------ */

static size_t build_data_packet (Transport *t, uint32_t seq, const uint8_t *payload, uint32_t payload_len, uint8_t *out, size_t out_cap)
{
  if (out_cap < WIRE_HEADER_SIZE + payload_len)
    return 0;

  WireHeader h;
  memset (&h, 0, sizeof (h));
  h.magic = WIRE_MAGIC;
  h.version = WIRE_VERSION;
  h.type = WIRE_DATA;
  h.flags = 0;
  h.conn_id = t->conn_id;
  h.seq = seq;
  h.ack = 0;
  h.ack_bits = 0;
  h.timestamp_us = now_us ();
  h.payload_len = payload_len;

  memcpy (out, &h, sizeof (h));
  memcpy (out + sizeof (h), payload, payload_len);
  return sizeof (h) + payload_len;
}

static bool parse_header (const uint8_t *buf, size_t len, WireHeader *out)
{
  if (len < WIRE_HEADER_SIZE)
    return false;

  memcpy (out, buf, sizeof (*out));
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

/* ------------------------------------------------------------------ *
 *  Sender path
 * ------------------------------------------------------------------ */

static void sender_close_file (Transport *t)
{
  if (t->send_file)
  {
    fclose (t->send_file);
    t->send_file = NULL;
  }

  t->send_in_progress = false;
}

static bool
sender_open_file (Transport *t)
{
  FILE *f = fopen (t->send_path, "rb");

  if (!f)
  {
    set_last_error (t, "cannot open file for sending");
    return false;
  }

  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);

  if (sz < 0)
  {
    fclose (f);
    set_last_error (t, "cannot determine file size");
    return false;
  }

  t->send_file = f;
  t->send_file_size = (uint64_t) sz;
  t->send_total_packets = (uint32_t) ((t->send_file_size + WIRE_MAX_PAYLOAD - 1) / WIRE_MAX_PAYLOAD);
  t->send_next_seq = 0;
  t->send_acked_seq = 0;
  t->send_in_progress = true;

  uint8_t ctrl_buf[WIRE_HEADER_SIZE + sizeof (ControlHeader) + 256];
  WireHeader ch;
  memset (&ch, 0, sizeof (ch));
  ch.magic = WIRE_MAGIC;
  ch.version = WIRE_VERSION;
  ch.type = WIRE_CONTROL;
  ch.conn_id = t->conn_id;
  ch.timestamp_us = now_us ();

  /* Strip path to basename for the filename. */
  const char *base = strrchr (t->send_path, '/');
  base = base ? base + 1 : t->send_path;

  ControlHeader ctl;

  memset (&ctl, 0, sizeof (ctl));
  ctl.control_type = CONTROL_FILE_HEADER;
  ctl.name_len = (uint32_t) strlen (base);
  ctl.file_size = t->send_file_size;
  ctl.total_packets = t->send_total_packets;

  ch.payload_len = (uint32_t) (sizeof (ctl) + ctl.name_len);

  memcpy (ctrl_buf, &ch, sizeof (ch));
  memcpy (ctrl_buf + sizeof (ch), &ctl, sizeof (ctl));
  memcpy (ctrl_buf + sizeof (ch) + sizeof (ctl), base, ctl.name_len);

  sendto (t->sock, ctrl_buf, sizeof (ch) + sizeof (ctl) + ctl.name_len, 0,
          (struct sockaddr *) &t->peer_addr, sizeof (t->peer_addr));

  return true;
}

/* Blast as many packets as the socket will take without blocking.
 * Stage 2e will replace this with a paced transmit. */
static void sender_pump (Transport *t)
{
  if (!t->send_in_progress || !t->send_file)
    return;

  if (!t->peer_known)
    return;

  uint8_t packet[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];

  for (;;)
  {
    if (t->send_next_seq >= t->send_total_packets)
    {
      /* Stage 2a: no completion signal, we just stop sending. */
      sender_close_file (t);
      if (t->cbs.on_progress)
        t->cbs.on_progress (1.0f, t->user);
      return;
    }

    uint8_t payload[WIRE_MAX_PAYLOAD];
    size_t n = fread (payload, 1, sizeof (payload), t->send_file);
    if (n == 0)
    {
      sender_close_file (t);
      if (t->cbs.on_progress)
        t->cbs.on_progress (1.0f, t->user);
      return;
    }

    size_t pkt_len = build_data_packet (t, t->send_next_seq,
                                        payload, (uint32_t) n,
                                        packet, sizeof (packet));
    if (pkt_len == 0)
    {
      set_last_error (t, "packet build failed");
      sender_close_file (t);
      return;
    }

    ssize_t rc = sendto (t->sock, packet, pkt_len, 0,
                         (struct sockaddr *) &t->peer_addr,
                         sizeof (t->peer_addr));
    if (rc < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        /* Socket buffer full; let poll() call us again. */
        return;
      }

      set_last_error (t, "sendto failed");
      sender_close_file (t);

      return;
    }

    t->send_next_seq++;

    if (t->cbs.on_progress && t->send_total_packets > 0)
    {
      float p = (float) t->send_next_seq /
                (float) t->send_total_packets;
      t->cbs.on_progress (p, t->user);
    }
  }
}

/* ------------------------------------------------------------------ *
 *  Receiver path
 * ------------------------------------------------------------------ */

static void receiver_close_file (Transport *t)
{
  if (t->recv_file)
  {
    fprintf(stderr, "receiver_close_file\n");
    fclose (t->recv_file);
    t->recv_file = NULL;
  }

  t->recv_in_progress = false;
  t->recv_expected_seq = 0;
  t->recv_highest_seq = 0;

  if (t->recv_window)
  {
    for (int i = 0; i < REASSEMBLY_WINDOW; i++)
    {
      t->recv_window[i].filled = false;
    }
  }
}

static bool receiver_open_file (Transport *t, const char *name, uint64_t file_size)
{
  receiver_close_file (t);

  fprintf(stderr, "receiver_open_file: name='%s' size=%llu\n",
    name, (unsigned long long)file_size);

  if (!t->recv_window)
  {
    t->recv_window = calloc (REASSEMBLY_WINDOW, sizeof (IncomingChunk));
    if (!t->recv_window)
    {
      set_last_error (t, "out of memory for reassembly window");
      return false;
    }
  }

  snprintf (t->recv_filename, sizeof (t->recv_filename), "%s", name);
  t->recv_file = fopen (name, "wb");

  if (!t->recv_file)
  {
    set_last_error (t, "cannot open output file");
    return false;
  }

  t->recv_file_size = file_size;
  t->recv_total_packets = (uint32_t) ((file_size + WIRE_MAX_PAYLOAD - 1) / WIRE_MAX_PAYLOAD);
  t->recv_expected_seq = 0;
  t->recv_highest_seq = 0;
  t->recv_in_progress = true;

  return true;
}

/* Flush any in-order chunks starting at recv_expected_seq. */
static void receiver_flush_in_order (Transport *t)
{
  fprintf(stderr, "receiver_flush_in_order: complete, fired on_file_received\n");

  while (t->recv_in_progress)
  {
    uint32_t idx = t->recv_expected_seq % REASSEMBLY_WINDOW;
    IncomingChunk *c = &t->recv_window[idx];
    if (!c->filled || c->seq != t->recv_expected_seq)
      break;

    fwrite (c->data, 1, c->payload_len, t->recv_file);
    c->filled = false;
    t->recv_expected_seq++;
  }

  /* Done? */
  if (t->recv_total_packets > 0 &&
      t->recv_expected_seq >= t->recv_total_packets)
  {
    fprintf(stderr, "receiver_flush_in_order: complete, firing on_file_received\n");
    fclose (t->recv_file);
    t->recv_file = NULL;
    t->recv_in_progress = false;

    if (t->cbs.on_file_received)
    {
      t->cbs.on_file_received (t->recv_filename,
                               t->recv_file_size, t->user);
    }
  }
}

static void receiver_handle_data (Transport *t, const WireHeader *h, const uint8_t *payload, size_t payload_len)
{
  /* Stage 2a: name the file after the first DATA packet's conn_id.
   * Stage 2b will use a proper CONTROL FILE_HEADER. */
  if (!t->recv_in_progress)
  {
    /* No CONTROL header seen yet; drop until one arrives. */
    return;
  }

  if (h->seq < t->recv_expected_seq)
  {
    return; /* duplicate */
  }

  uint32_t idx = h->seq % REASSEMBLY_WINDOW;
  IncomingChunk *c = &t->recv_window[idx];

  /* Stage 2a has no flow control, so packet arrival can outrun the
   * window. Guard against overwriting a slot still waiting to flush. */
  if (c->filled && c->seq != h->seq)
  {
    /* Slot is occupied by a different seq: out of window. Drop. */
    return;
  }

  c->seq = h->seq;
  c->payload_len = (uint32_t) payload_len;
  memcpy (c->data, payload, payload_len);
  c->filled = true;

  if (h->seq > t->recv_highest_seq)
    t->recv_highest_seq = h->seq;

  receiver_flush_in_order (t);
}

/* ------------------------------------------------------------------ *
 *  Incoming packet dispatch
 * ------------------------------------------------------------------ */

static void handle_incoming(Transport *t)
{
    uint8_t buf[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];

    for (;;)
    {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(t->sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK) return;
          fprintf(stderr, "recvfrom failed: %s\n", strerror(errno));

          return;
        }

        char src[32];
        inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
        fprintf(stderr, "recv %zd bytes from %s:%u\n",
                n, src, ntohs(from.sin_port));

        if (!t->peer_known)
        {
          t->peer_addr = from;
          t->peer_known = true;
          fprintf(stderr, "  learned peer\n");
        }

        WireHeader h;
        if (!parse_header(buf, (size_t)n, &h))
        {
          fprintf(stderr, "  parse_header failed (magic=%08x ver=%u type=%u plen=%u n=%zd)\n",
                  ((WireHeader *)buf)->magic,
                  ((WireHeader *)buf)->version,
                  ((WireHeader *)buf)->type,
                  ((WireHeader *)buf)->payload_len,
                  n);
          continue;
        }

        fprintf(stderr, "  type=%u conn_id=%08x seq=%u ack=%u plen=%u ts=%llu\n",
                h.type, h.conn_id, h.seq, h.ack, h.payload_len,
                (unsigned long long)h.timestamp_us);

        if (t->sender_conn_id == 0)
        {
          t->sender_conn_id = h.conn_id;
          fprintf(stderr, "  learned sender conn_id: %08x\n", h.conn_id);
        } else if (h.conn_id != t->sender_conn_id)
        {
          fprintf(stderr, "  drop: conn_id mismatch (have %08x, got %08x)\n",
                  t->sender_conn_id, h.conn_id);
          continue;
        }

        switch (h.type)
        {
          case WIRE_DATA:
              fprintf(stderr, "  DATA seq=%u plen=%u\n", h.seq, h.payload_len);
              receiver_handle_data(t, &h, buf + WIRE_HEADER_SIZE, h.payload_len);
              break;

          case WIRE_ACK:
              fprintf(stderr, "  ACK ack=%u bits=%08x\n", h.ack, h.ack_bits);
              break;

          case WIRE_CONTROL: {
              if (h.payload_len < sizeof(ControlHeader))
              {
                fprintf(stderr, "  CONTROL too short\n");
                break;
              }

              const uint8_t *p = buf + WIRE_HEADER_SIZE;
              ControlHeader ctl;
              memcpy(&ctl, p, sizeof(ctl));
              fprintf(stderr, "  CONTROL type=%u name_len=%u file_size=%llu total=%u\n",
                      ctl.control_type, ctl.name_len,
                      (unsigned long long)ctl.file_size, ctl.total_packets);

              if (ctl.control_type == CONTROL_FILE_HEADER)
              {
                if (ctl.name_len > 250)
                {
                  fprintf(stderr, "  filename too long\n");
                  break;
                }

                if (h.payload_len < sizeof(ctl) + ctl.name_len)
                {
                  fprintf(stderr, "  CONTROL truncated\n");
                  break;
                }

                char name[256];
                memcpy(name, p + sizeof(ctl), ctl.name_len);
                name[ctl.name_len] = '\0';
                receiver_open_file(t, name, ctl.file_size);
              }
              break;
          }

          default:
              fprintf(stderr, "  unknown type\n");
              break;
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------ */

Transport * transport_create (const char *bind_host, int bind_port, const char *peer_host, int peer_port, const TransportCallbacks *cbs, void *user)
{
  Transport *t = calloc (1, sizeof (*t));
  if (!t)
    return NULL;

  t->user = user;
  if (cbs)
    t->cbs = *cbs;

  t->sock = make_udp_socket (bind_host, bind_port, &t->bind_addr);

  if (t->sock < 0)
  {
    free (t);
    return NULL;
  }

  t->sender_conn_id = 0;

  /* Peer is only known up front if peer_port != 0. In receive mode
   * the caller passes port 0 and we learn the peer on first packet. */
  if (peer_port != 0)
  {
    memset (&t->peer_addr, 0, sizeof (t->peer_addr));
    t->peer_addr.sin_family = AF_INET;
    t->peer_addr.sin_port = htons ((uint16_t) peer_port);

    if (inet_pton (AF_INET, peer_host, &t->peer_addr.sin_addr) != 1)
    {
      close (t->sock);
      free (t);
      return NULL;
    }

    t->peer_known = true;
  }

  /* conn_id is random so two runs don't cross-talk on the same port. */
  t->conn_id = (uint32_t) now_us () ^ (uint32_t) getpid ();

  return t;
}

void transport_destroy (Transport *t)
{
  if (!t)
    return;
  sender_close_file (t);
  receiver_close_file (t);
  if (t->recv_window)
    free (t->recv_window);
  if (t->sock >= 0)
    close (t->sock);
  free (t);
}

bool transport_queue_send (Transport *t, const char *path)
{
  if (!t || !path || !*path)
    return false;
  if (t->send_in_progress)
    return false;

  snprintf (t->send_path, sizeof (t->send_path), "%s", path);

  if (!sender_open_file (t))
    return false;

  return true;
}

void transport_poll (Transport *t)
{
  if (!t || t->cancelled)
    return;

  handle_incoming (t);
  sender_pump (t);
}

void transport_cancel (Transport *t)
{
  if (!t)
    return;

  t->cancelled = true;
  sender_close_file (t);
  receiver_close_file (t);
}

float transport_get_progress (const Transport *t)
{
  if (!t)
    return 0.0f;

  if (!t->send_in_progress)
    return 1.0f;

  if (t->send_total_packets == 0)
    return 0.0f;

  return (float) t->send_next_seq / (float) t->send_total_packets;
}

uint64_t transport_get_pacing_rate (const Transport *t)
{
  (void) t;

  return 0;
}

uint64_t transport_get_cwnd (const Transport *t)
{
  (void) t;

  return 0;
}

uint64_t transport_get_min_rtt_us (const Transport *t)
{
  (void) t;

  return 0;
}

const char * transport_get_bbr_state (const Transport *t)
{
  (void) t;

  return "N/A";
}

const char * transport_get_last_error (const Transport *t)
{
  return t ? t->last_error : "";
}

