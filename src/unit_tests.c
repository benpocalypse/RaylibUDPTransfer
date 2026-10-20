#include "transport_internal.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(test) do { \
    printf("  Running %s... ", #test); \
    tests_run++; \
    if (test()) { \
        printf("PASS\n"); \
        tests_passed++; \
    } else { \
        printf("FAIL\n"); \
    } \
} while (0)

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

static bool test_parse_header_valid(void)
{
  uint8_t buf[WIRE_HEADER_SIZE];
  WireHeader *h = (WireHeader *)buf;
  memset(h, 0, sizeof(WireHeader));
  h->magic = WIRE_MAGIC;
  h->version = WIRE_VERSION;
  h->type = WIRE_DATA;
  h->payload_len = 0;

  WireHeader out;

  return parse_header(buf, sizeof(buf), &out) == true;
}

static bool test_parse_header_bad_magic(void)
{
  uint8_t buf[WIRE_HEADER_SIZE];
  WireHeader *h = (WireHeader *)buf;

  memset(h, 0, sizeof(WireHeader));
  h->magic = 0x12345678; // Wrong magic
  h->version = WIRE_VERSION;

  WireHeader out;

  return parse_header(buf, sizeof(buf), &out) == false;
}

static bool test_build_data_packet(void)
{
  Transport *t = transport_create_for_test();
  uint8_t payload[10] = {1,2,3,4,5,6,7,8,9,10};
  uint8_t out[WIRE_HEADER_SIZE + 10];

  size_t len = build_data_packet(t, 42, payload, 10, out, sizeof(out));
  transport_destroy(t);

  if (len != WIRE_HEADER_SIZE + 10)
    return false;

  WireHeader *h = (WireHeader *)out;

  if (h->magic != WIRE_MAGIC)
    return false;

  if (h->seq != 42)
    return false;

  if (h->payload_len != 10)
    return false;

  if (memcmp(out + WIRE_HEADER_SIZE, payload, 10) != 0)
    return false;

  return true;
}

static bool test_ring_buffer_flow_control(void)
{
  /* Test that we don't exceed the ring buffer size */
  Transport *t = transport_create_for_test();

  // Simulate sending 300 packets (more than OUTGOING_QUEUE_SIZE)
  for (uint32_t i = 0; i < 300; i++)
  {
      OutgoingPacket *op = &t->outgoing[i % OUTGOING_QUEUE_SIZE];
      op->seq = i;
      op->in_use = true;
  }

  // The last packet should overwrite the first one
  // Slot 0 (originally seq 0) should now hold seq 256
  if (t->outgoing[0].seq != 256)
  {
    transport_destroy(t);

    return false;
  }

  transport_destroy(t);

  return true;
}

static bool test_ack_advancement(void)
{
  Transport *t = transport_create_for_test();

  t->send_total_packets = 3;
  t->send_acked_seq = 0;

  // Packets 0, 1, 2 are in the ring, all in_use
  for (int i = 0; i < 3; i++)
  {
    t->outgoing[i].seq = i;
    t->outgoing[i].in_use = true;
  }

  // Simulate receiving an ACK for packet 0
  t->outgoing[0].in_use = false;

  // Advance contiguous
  while (t->send_acked_seq < t->send_total_packets)
  {
    OutgoingPacket *c = &t->outgoing[t->send_acked_seq % OUTGOING_QUEUE_SIZE];

    if (c->seq != t->send_acked_seq || c->in_use)
      break;

    t->send_acked_seq++;
  }

  bool result = (t->send_acked_seq == 1);
  transport_destroy(t);

  return result;
}

/* ------------------------------------------------------------------ *
 * Main entry point
 * ------------------------------------------------------------------ */

int run_unit_tests(void)
{
  printf("\n=== UDP Transfer Unit Tests ===\n\n");

  RUN_TEST(test_parse_header_valid);
  RUN_TEST(test_parse_header_bad_magic);
  RUN_TEST(test_build_data_packet);
  RUN_TEST(test_ring_buffer_flow_control);
  RUN_TEST(test_ack_advancement);

  printf("\nResults: %d/%d tests passed\n", tests_passed, tests_run);

  return (tests_passed == tests_run) ? 0 : 1;
}

