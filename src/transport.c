#include "transport_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int make_udp_socket(const char *host, int port,
                           struct sockaddr_in *out_addr)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (fd < 0)
    return -1;

  int flags = fcntl(fd, F_GETFL, 0);

  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
  {
      close(fd); return -1;
  }

  int rcvbuf = 4 * 1024 * 1024;

  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  memset(out_addr, 0, sizeof(*out_addr));
  out_addr->sin_family = AF_INET;
  out_addr->sin_port   = htons((uint16_t)port);

  if (inet_pton(AF_INET, host, &out_addr->sin_addr) != 1)
  {
    close(fd); return -1;
  }

  if (bind(fd, (struct sockaddr *)out_addr, sizeof(*out_addr)) < 0)
  {
    close(fd); return -1;
  }

  return fd;
}

Transport *transport_create(const char *bind_host, int bind_port,
                            const char *peer_host, int peer_port,
                            const TransportCallbacks *cbs, void *user)
{
  Transport *t = calloc(1, sizeof(*t));

  if (!t)
    return NULL;

  t->outgoing = calloc(OUTGOING_QUEUE_SIZE, sizeof(OutgoingPacket));

  if (!t->outgoing)
  {
    free(t);
    return NULL;
  }

  t->user = user;

  if (cbs)
    t->cbs = *cbs;

  t->sock = make_udp_socket(bind_host, bind_port, &t->bind_addr);

  if (t->sock < 0)
  {
    free(t->outgoing);
    free(t);

    return NULL;
  }

  t->sender_conn_id = 0;

  if (peer_port != 0)
  {
    memset(&t->peer_addr, 0, sizeof(t->peer_addr));
    t->peer_addr.sin_family = AF_INET;
    t->peer_addr.sin_port   = htons((uint16_t)peer_port);

    if (inet_pton(AF_INET, peer_host, &t->peer_addr.sin_addr) != 1)
    {
      close(t->sock); free(t->outgoing); free(t);

      return NULL;
    }

    t->peer_known = true;
  }

  t->conn_id = (uint32_t)now_us() ^ (uint32_t)getpid();

  return t;
}

Transport *transport_create_for_test(void)
{
  Transport *t = calloc(1, sizeof(*t));

  if (!t)
    return NULL;

  t->outgoing = calloc(OUTGOING_QUEUE_SIZE, sizeof(OutgoingPacket));

  if (!t->outgoing)
  {
    free(t);
    return NULL;
  }

  t->sock = -1; // No real socket
  t->conn_id = 0xDEADBEEF;

  return t;
}

void transport_destroy(Transport *t)
{
  if (!t)
    return;

  sender_close_file(t);
  receiver_close_file(t);

  if (t->recv_window)
    free(t->recv_window);

  if (t->outgoing)
    free(t->outgoing);

  if (t->sock >= 0)
    close(t->sock);

  free(t);
}

bool transport_queue_send(Transport *t, const char *path)
{
  if (!t || !path || !*path)
    return false;

  if (t->send_in_progress)
    return false;

  snprintf(t->send_path, sizeof(t->send_path), "%s", path);

  return sender_open_file(t);
}

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
      return;
    }

    if (!t->peer_known)
    {
      t->peer_addr = from;
      t->peer_known = true;
    }

    WireHeader h;

    if (!parse_header(buf, (size_t)n, &h))
      continue;

    if (t->sender_conn_id == 0)
    {
      t->sender_conn_id = h.conn_id;
    }
    else if (h.conn_id != t->sender_conn_id)
    {
      continue;
    }

    switch (h.type)
    {
      case WIRE_DATA:
          receiver_handle_data(t, &h, buf + WIRE_HEADER_SIZE, h.payload_len);
          break;

      case WIRE_ACK:
          sender_handle_ack(t, &h);
          break;

      case WIRE_CONTROL:
      {
        if (h.payload_len < sizeof(ControlHeader))
          break;

        const uint8_t *p = buf + WIRE_HEADER_SIZE;
        ControlHeader ctl;

        memcpy(&ctl, p, sizeof(ctl));

        if (ctl.control_type != CONTROL_FILE_HEADER)
          break;

        if (ctl.name_len > 250)
          break;

        if (h.payload_len < sizeof(ctl) + ctl.name_len)
          break;

        char name[256];

        memcpy(name, p + sizeof(ctl), ctl.name_len);
        name[ctl.name_len] = '\0';
        receiver_open_file(t, name, ctl.file_size);

        break;
      }

      default:
        break;
    }
  }
}

void transport_poll(Transport *t)
{
  if (!t || t->cancelled)
    return;

  handle_incoming(t);
  sender_pump(t);
  sender_check_timeouts(t);
}

void transport_cancel(Transport *t)
{
  if (!t)
    return;

  t->cancelled = true;
  sender_close_file(t);
  receiver_close_file(t);
}

float transport_get_progress(const Transport *t)
{
  if (!t || t->send_total_packets == 0)
    return 0.0f;
  else
    return (float)t->send_acked_seq / (float)t->send_total_packets;
}

bool transport_send_complete(const Transport *t)
{
  return t ? t->send_completed_ok : false;
}

uint64_t transport_get_pacing_rate(const Transport *t)
{
  return t ? t->pacing_rate : 0;
}

uint64_t transport_get_cwnd(const Transport *t)
{
    /* Stage 2f will compute this from BBR. For now, flow-control equivalent. */
    (void)t;

    return OUTGOING_QUEUE_SIZE;
}

uint64_t transport_get_min_rtt_us(const Transport *t)
{
  (void)t;
  return 0;  /* Stage 2f: from BBR */
}

const char *transport_get_bbr_state(const Transport *t)
{
  (void)t;
  return "N/A";  /* Stage 2f: from BBR */
}

const char *transport_get_last_error(const Transport *t)
{
    return t ? t->last_error : "";
}

