#include "viewmodel.h"
#include "transport.h"
#include <stdio.h>
#include <string.h>

void vm_init(ViewModel *vm) {
    model_init(&vm->model);
    vm->transport = NULL;
    vm->send_enabled = true;
    vm->cancel_enabled = false;
    vm->ip_edit_mode = false;
    vm->path_edit_mode = false;
    strncpy(vm->progress_text, "0%", sizeof(vm->progress_text) - 1);
    strncpy(vm->state_text, "Idle", sizeof(vm->state_text) - 1);
}

static const char *state_to_string(TransferState s) {
    switch (s) {
        case STATE_IDLE:      return "Idle";
        case STATE_SENDING:   return "Sending";
        case STATE_RECEIVING: return "Receiving";
        case STATE_ERROR:     return "Error";
        case STATE_DONE:      return "Done";
    }
    return "Unknown";
}

void vm_update(ViewModel *vm) {
    snprintf(vm->progress_text, sizeof(vm->progress_text),
             "%d%%", (int)(vm->model.progress * 100.0f));
    strncpy(vm->state_text, state_to_string(vm->model.state),
            sizeof(vm->state_text) - 1);

    vm->send_enabled = (vm->model.state == STATE_IDLE ||
                        vm->model.state == STATE_DONE);
    vm->cancel_enabled = (vm->model.state == STATE_SENDING ||
                          vm->model.state == STATE_RECEIVING);

    if (vm->transport) {
        vm->model.progress = transport_get_progress(vm->transport);
    }
}

static void on_progress(float p, void *user) {
    ViewModel *vm = (ViewModel *)user;
    vm->model.progress = p;
}

static void on_file_received(const char *name, uint64_t size, void *user) {
    ViewModel *vm = (ViewModel *)user;
    model_add_received(&vm->model, name, size);
    strncpy(vm->model.status_message, "File received",
            sizeof(vm->model.status_message) - 1);
}

static void on_error(const char *message, void *user) {
    ViewModel *vm = (ViewModel *)user;
    strncpy(vm->model.status_message, message,
            sizeof(vm->model.status_message) - 1);
    vm->model.state = STATE_ERROR;
}

void vm_start_send(ViewModel *vm) {
    if (vm->model.file_path[0] == '\0') {
        strncpy(vm->model.status_message, "No file selected",
                sizeof(vm->model.status_message) - 1);
        vm->model.state = STATE_ERROR;
        return;
    }

    if (!vm->transport) {
        TransportCallbacks cbs = {
            .on_progress = on_progress,
            .on_file_received = on_file_received,
            .on_error = on_error
        };
        vm->transport = transport_create(vm->model.target_ip,
                                         vm->model.target_port,
                                         &cbs, vm);
        if (!vm->transport) {
            strncpy(vm->model.status_message, "Failed to open transport",
                    sizeof(vm->model.status_message) - 1);
            vm->model.state = STATE_ERROR;
            return;
        }
    }

    vm->model.state = STATE_SENDING;
    strncpy(vm->model.status_message, "Sending...",
            sizeof(vm->model.status_message) - 1);

    transport_queue_send(vm->transport, vm->model.file_path);
}

void vm_cancel(ViewModel *vm) {
    if (vm->transport) {
        transport_cancel(vm->transport);
        transport_destroy(vm->transport);
        vm->transport = NULL;
    }
    vm->model.state = STATE_IDLE;
}

