#include "model.h"
#include <string.h>
#include <stdio.h>

void model_init(TransferModel *m) {
    memset(m, 0, sizeof(*m));
    strncpy(m->target_ip, "127.0.0.1", MAX_IP_LEN - 1);
    m->target_port = 3000;
    m->progress = 0.0f;
    m->state = STATE_IDLE;
    strncpy(m->status_message, "Idle", sizeof(m->status_message) - 1);
}

void model_add_received(TransferModel *m, const char *name, uint64_t size) {
    if (m->received_count >= MAX_RECEIVED) return;
    ReceivedFile *rf = &m->received[m->received_count++];
    strncpy(rf->name, name, MAX_FILENAME - 1);
    rf->size = size;
    rf->complete = true;
}

