#include "transport.h"
#include "unit_tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Global flag used to break out of the poll loop. */
static volatile int g_stop = 0;
static volatile int g_complete = 0;
static volatile int g_failed = 0;

static void handle_signal (int sig)
{
  (void)sig;
  g_stop = 1;
}

static void print_usage (const char *argv0)
{
  fprintf (stderr,
           "usage:\n"
           "  %s send <ip> <port> <filename>\n"
           "  %s receive <port>\n"
           "  %s --run-unit-tests\n"
           "\n"
           "examples:\n"
           "  %s send 127.0.0.1 3000 ./colorpicker.sh\n"
           "  %s receive 3000\n",
           argv0, argv0, argv0, argv0, argv0);
}

/* --------------------------------------------------------------------- *
 *  Callbacks: these update the shared flags and print progress.
 * --------------------------------------------------------------------- */

static void on_progress (float p, void *user)
{
  (void)user;
  /* Print only on 10% boundaries to avoid flooding the terminal. */
  static int last_decile = -1;
  int decile = (int)(p * 10.0f);
  if (decile != last_decile)
  {
    last_decile = decile;
    fprintf (stderr, "progress: %3d%%\n", (int)(p * 100.0f));
  }
}

static void on_file_received (const char *name, uint64_t size, void *user)
{
  (void)user;
  printf ("received: %s (%llu bytes)\n", name, (unsigned long long)size);
}

static void on_error (const char *message, void *user)
{
  (void)user;
  fprintf (stderr, "error: %s\n", message);
  g_failed = 1;
}

/* --------------------------------------------------------------------- *
 *  Polling thread shared by sender and receiver.
 * --------------------------------------------------------------------- */

static void *poll_thread (void *arg)
{
  Transport *t = (Transport *)arg;
  while (!g_stop)
  {
    transport_poll (t);
    usleep (500);
    if (transport_send_complete (t))
    {
      g_complete = 1;
      break;
    }
  }
  return NULL;
}

/* --------------------------------------------------------------------- *
 *  send <ip> <port> <filename>
 * --------------------------------------------------------------------- */

static int run_send (const char *ip, const char *port_str, const char *path)
{
  /* Validate port. */
  char *end = NULL;
  long port = strtol (port_str, &end, 10);
  if (end == port_str || *end != '\0' || port <= 0 || port > 65535)
  {
    fprintf (stderr, "invalid port: '%s'\n", port_str);
    return 1;
  }

  /* Validate IP. */
  struct in_addr parsed;
  if (inet_pton (AF_INET, ip, &parsed) != 1)
  {
    fprintf (stderr, "invalid IP address: '%s'\n", ip);
    return 1;
  }

  /* Validate file. */
  struct stat st;
  if (stat (path, &st) != 0)
  {
    fprintf (stderr, "cannot stat '%s': %s\n", path, strerror (errno));
    return 1;
  }
  if (!S_ISREG (st.st_mode))
  {
    fprintf (stderr, "'%s' is not a regular file\n", path);
    return 1;
  }
  if (st.st_size == 0)
  {
    fprintf (stderr, "'%s' is empty\n", path);
    return 1;
  }

  TransportCallbacks cbs =
  {
    .on_progress      = on_progress,
    .on_file_received = on_file_received,
    .on_error         = on_error,
  };

  Transport *t = transport_create ("0.0.0.0", 0,
                                    ip, (int)port,
                                    &cbs, NULL);
  if (!t)
  {
    fprintf (stderr, "failed to create transport\n");
    return 1;
  }

  if (!transport_queue_send (t, path))
  {
    fprintf (stderr, "failed to queue '%s' for sending\n", path);
    transport_destroy (t);
    return 1;
  }

  fprintf (stderr, "sending '%s' (%lld bytes) to %s:%ld\n",
           path, (long long)st.st_size, ip, port);

  pthread_t tid;
  if (pthread_create (&tid, NULL, poll_thread, t) != 0)
  {
    fprintf (stderr, "failed to start polling thread\n");
    transport_destroy (t);
    return 1;
  }

  pthread_join (tid, NULL);

  int rc = 1;
  if (g_failed)
  {
    fprintf (stderr, "transfer failed\n");
  }
  else if (g_complete)
  {
    fprintf (stderr, "transfer complete\n");
    rc = 0;
  }
  else
  {
    fprintf (stderr, "transfer interrupted\n");
  }

  transport_destroy (t);
  return rc;
}

/* --------------------------------------------------------------------- *
 *  receive <port>
 * --------------------------------------------------------------------- */

static int run_receive (const char *port_str)
{
  char *end = NULL;
  long port = strtol (port_str, &end, 10);
  if (end == port_str || *end != '\0' || port <= 0 || port > 65535)
  {
    fprintf (stderr, "invalid port: '%s'\n", port_str);
    return 1;
  }

  TransportCallbacks cbs =
  {
    .on_progress      = NULL,
    .on_file_received = on_file_received,
    .on_error         = on_error,
  };

  Transport *t = transport_create ("0.0.0.0", (int)port,
                                    "127.0.0.1", 0,
                                    &cbs, NULL);
  if (!t)
  {
    fprintf (stderr, "failed to create transport on port %ld\n", port);
    return 1;
  }

  fprintf (stderr, "receiving on 0.0.0.0:%ld\n", port);

  pthread_t tid;
  if (pthread_create (&tid, NULL, poll_thread, t) != 0)
  {
    fprintf (stderr, "failed to start polling thread\n");
    transport_destroy (t);
    return 1;
  }

  pthread_join (tid, NULL);

  transport_destroy (t);
  return g_failed ? 1 : 0;
}

/* --------------------------------------------------------------------- *
 *  main
 * --------------------------------------------------------------------- */

int main (int argc, char **argv)
{
  signal (SIGINT,  handle_signal);
  signal (SIGTERM, handle_signal);

  if (argc < 2)
  {
    print_usage (argv[0]);
    return 1;
  }

  if (strcmp (argv[1], "--run-unit-tests") == 0)
  {
    return run_unit_tests ();
  }

  if (strcmp (argv[1], "send") == 0)
  {
    if (argc != 5)
    {
      fprintf (stderr, "usage: %s send <ip> <port> <filename>\n", argv[0]);
      return 1;
    }
    return run_send (argv[2], argv[3], argv[4]);
  }

  if (strcmp (argv[1], "receive") == 0)
  {
    if (argc != 3)
    {
      fprintf (stderr, "usage: %s receive <port>\n", argv[0]);
      return 1;
    }
    return run_receive (argv[2]);
  }

  fprintf (stderr, "unknown subcommand: '%s'\n", argv[1]);
  print_usage (argv[0]);
  return 1;
}
