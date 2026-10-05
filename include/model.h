#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_IP_LEN 64
#define MAX_PATH_LEN 512
#define MAX_RECEIVED 32
#define MAX_FILENAME 256
#define CHUNK_SIZE 8192

typedef enum {
    STATE_IDLE,
    STATE_SENDING,
    STATE_RECEIVING,
    STATE_ERROR,
    STATE_DONE
} TransferState;

typedef struct {
    char name[MAX_FILENAME];
    uint64_t size;
    bool complete;
} ReceivedFile;

typedef struct {
    char target_ip[MAX_IP_LEN];
    int target_port;
    char file_path[MAX_PATH_LEN];

    ReceivedFile received[MAX_RECEIVED];
    int received_count;

    float progress;
    TransferState state;
    char status_message[256];
} TransferModel;

void model_init(TransferModel *m);
void model_add_received(TransferModel *m, const char *name, uint64_t size);

#endif

