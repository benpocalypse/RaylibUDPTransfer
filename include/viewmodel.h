#ifndef VIEWMODEL_H
#define VIEWMODEL_H

#include "model.h"

typedef struct {
    TransferModel model;

    char progress_text[64];
    char state_text[64];
    bool send_enabled;
    bool cancel_enabled;

    // raygui edit-mode state for text fields
    bool ip_edit_mode;
    bool path_edit_mode;

    void *transport;
} ViewModel;

void vm_init(ViewModel *vm);
void vm_update(ViewModel *vm);
void vm_start_send(ViewModel *vm);
void vm_cancel(ViewModel *vm);

#endif

