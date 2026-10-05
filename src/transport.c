#include "transport.h"
#include <enet/enet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_FILENAME 256
#define CHUNK_SIZE 8192  // ENet will fragment if larger than MTU

// Channel definitions
#define CHANNEL_CONTROL 0   // Reliable: file metadata, commands
#define CHANNEL_DATA    1   // Reliable: file data chunks

typedef enum {
    MSG_FILE_HEADER = 1,
    MSG_FILE_DATA,
    MSG_FILE_COMPLETE
} MessageType;

// Wire header for file metadata
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t name_len;
    uint64_t file_size;
    // filename follows
} FileHeader;

struct Transport {
    ENetHost *host;
    ENetPeer *peer;
    bool is_server;
    bool connected;
    bool cancelled;
    float progress;

    // Receiving state
    FILE *recv_file;
    char recv_filename[MAX_FILENAME];
    uint64_t recv_expected;
    uint64_t recv_received;

    // Sending state
    FILE *send_file;
    uint64_t send_total;
    uint64_t send_sent;

    // Queued file to send
    char queued_path[512];
    bool has_queued_send;

    TransportCallbacks cbs;
    void *user;
};

static bool send_file_chunk(Transport *t);

Transport *transport_create(const char *host, int port,
                            const TransportCallbacks *cbs, void *user) {
    if (enet_initialize() != 0) {
        return NULL;
    }

    Transport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    t->cbs = *cbs;
    t->user = user;
    t->progress = 0.0f;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = (enet_uint16)port;

    // Create a server host bound to the local port.
    // We act as the "server" side locally; the remote side connects to us
    // through the tunnel. This avoids needing to know the remote's address.
    t->host = enet_host_create(&address,
                               1,      // allow 1 peer
                               2,      // 2 channels: control + data
                               0, 0);  // unlimited bandwidth
    if (!t->host) {
        free(t);
        return NULL;
    }

    t->is_server = true;
    return t;
}

void transport_destroy(Transport *t) {
    if (!t) return;
    if (t->peer) {
        enet_peer_disconnect(t->peer, 0);
        // Give a brief moment for disconnect to flush
        ENetEvent event;
        while (enet_host_service(t->host, &event, 100) > 0) {
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
    }
    if (t->recv_file) fclose(t->recv_file);
    if (t->send_file) fclose(t->send_file);
    if (t->host) enet_host_destroy(t->host);
    enet_deinitialize();
    free(t);
}

bool transport_queue_send(Transport *t, const char *path) {
    if (!t || !t->connected) return false;
    strncpy(t->queued_path, path, sizeof(t->queued_path) - 1);
    t->has_queued_send = true;
    return true;
}

static bool send_file_header(Transport *t) {
    FILE *f = fopen(t->queued_path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *basename = strrchr(t->queued_path, '/');
    basename = basename ? basename + 1 : t->queued_path;
    uint32_t name_len = (uint32_t)strlen(basename);

    size_t header_size = sizeof(FileHeader) + name_len;
    ENetPacket *packet = enet_packet_create(NULL, header_size,
                                             ENET_PACKET_FLAG_RELIABLE);
    if (!packet) { fclose(f); return false; }

    FileHeader *hdr = (FileHeader *)packet->data;
    hdr->type = MSG_FILE_HEADER;
    hdr->name_len = name_len;
    hdr->file_size = size;
    memcpy(packet->data + sizeof(FileHeader), basename, name_len);

    if (enet_peer_send(t->peer, CHANNEL_CONTROL, packet) < 0) {
        enet_packet_destroy(packet);
        fclose(f);
        return false;
    }

    t->send_file = f;
    t->send_total = size;
    t->send_sent = 0;
    return true;
}

static bool send_file_chunk(Transport *t) {
    if (!t->send_file) return false;

    char buf[CHUNK_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), t->send_file);
    if (n == 0) {
        // Send completion marker
        ENetPacket *done = enet_packet_create(NULL, 1,
                                               ENET_PACKET_FLAG_RELIABLE);
        done->data[0] = MSG_FILE_COMPLETE;
        enet_peer_send(t->peer, CHANNEL_DATA, done);
        fclose(t->send_file);
        t->send_file = NULL;
        return false;  // done
    }

    // Build data packet: 1-byte type + payload
    ENetPacket *packet = enet_packet_create(NULL, 1 + n,
                                             ENET_PACKET_FLAG_RELIABLE);
    packet->data[0] = MSG_FILE_DATA;
    memcpy(packet->data + 1, buf, n);
    enet_peer_send(t->peer, CHANNEL_DATA, packet);

    t->send_sent += n;
    t->progress = (float)t->send_sent / (float)t->send_total;
    if (t->cbs.on_progress)
        t->cbs.on_progress(t->progress, t->user);
    return true;
}

static void handle_control_packet(Transport *t, ENetPacket *packet) {
    if (packet->dataLength < 1) return;
    uint8_t type = packet->data[0];

    if (type == MSG_FILE_HEADER) {
        if (packet->dataLength < sizeof(FileHeader)) return;
        FileHeader *hdr = (FileHeader *)packet->data;
        if (packet->dataLength < sizeof(FileHeader) + hdr->name_len) return;

        memcpy(t->recv_filename, packet->data + sizeof(FileHeader),
               hdr->name_len);
        t->recv_filename[hdr->name_len] = '\0';

        t->recv_file = fopen(t->recv_filename, "wb");
        t->recv_expected = hdr->file_size;
        t->recv_received = 0;
    }
}

static void handle_data_packet(Transport *t, ENetPacket *packet) {
    if (packet->dataLength < 1) return;
    uint8_t type = packet->data[0];

    if (type == MSG_FILE_DATA && t->recv_file) {
        size_t payload = packet->dataLength - 1;
        fwrite(packet->data + 1, 1, payload, t->recv_file);
        t->recv_received += payload;
    } else if (type == MSG_FILE_COMPLETE && t->recv_file) {
        fclose(t->recv_file);
        t->recv_file = NULL;
        if (t->cbs.on_file_received)
            t->cbs.on_file_received(t->recv_filename,
                                     t->recv_expected, t->user);
    }
}

void transport_poll(Transport *t) {
    if (!t || t->cancelled) return;

    ENetEvent event;
    while (enet_host_service(t->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            t->peer = event.peer;
            t->connected = true;
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            if (event.channelID == CHANNEL_CONTROL)
                handle_control_packet(t, event.packet);
            else if (event.channelID == CHANNEL_DATA)
                handle_data_packet(t, event.packet);
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            t->connected = false;
            t->peer = NULL;
            if (t->cbs.on_error)
                t->cbs.on_error("Peer disconnected", t->user);
            break;

        default:
            break;
        }
    }

    // Process queued send
    if (t->has_queued_send && t->connected && !t->send_file) {
        t->has_queued_send = false;
        if (!send_file_header(t)) {
            if (t->cbs.on_error)
                t->cbs.on_error("Failed to open file", t->user);
        }
    }

    // Continue sending chunks (rate-limited by ENet's flow control)
    if (t->send_file) {
        send_file_chunk(t);
    }
}

float transport_get_progress(const Transport *t) {
    return t ? t->progress : 0.0f;
}

void transport_cancel(Transport *t) {
    if (!t) return;
    t->cancelled = true;
    if (t->recv_file) { fclose(t->recv_file); t->recv_file = NULL; }
    if (t->send_file) { fclose(t->send_file); t->send_file = NULL; }
}

