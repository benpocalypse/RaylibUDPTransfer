#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct Transport Transport;

// Callbacks invoked from the transport thread
typedef struct {
    void (*on_progress)(float progress, void *user);
    void (*on_file_received)(const char *name, uint64_t size, void *user);
    void (*on_error)(const char *message, void *user);
} TransportCallbacks;

Transport *transport_create(const char *host, int port,
                            const TransportCallbacks *cbs, void *user);
void transport_destroy(Transport *t);

// Non-blocking: queues a file for sending
bool transport_queue_send(Transport *t, const char *path);

// Must be called regularly from the transport thread
void transport_poll(Transport *t);

float transport_get_progress(const Transport *t);
void transport_cancel(Transport *t);

#endif
