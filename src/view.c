#include "view.h"
#include "raygui.h"
#include "raylib.h"
#include <stdio.h>
#include <string.h>

void view_draw(ViewModel *vm) {
    int y = 20;

    GuiLabel((Rectangle){20, y, 200, 20}, "Target IP:");
    if (GuiTextBox((Rectangle){20, y + 25, 200, 30},
                   vm->model.target_ip, MAX_IP_LEN,
                   vm->ip_edit_mode)) {
        vm->ip_edit_mode = !vm->ip_edit_mode;
    }

    GuiLabel((Rectangle){240, y, 200, 20}, "File path:");
    if (GuiTextBox((Rectangle){240, y + 25, 380, 30},
                   vm->model.file_path, MAX_PATH_LEN,
                   vm->path_edit_mode)) {
        vm->path_edit_mode = !vm->path_edit_mode;
    }

    y += 70;

    if (GuiButton((Rectangle){20, y, 100, 30}, "Send")) {
        if (vm->send_enabled) vm_start_send(vm);
    }

    if (GuiButton((Rectangle){130, y, 100, 30}, "Cancel")) {
        if (vm->cancel_enabled) vm_cancel(vm);
    }

    y += 45;

    GuiLabel((Rectangle){20, y, 100, 20}, "Progress:");
    GuiProgressBar((Rectangle){120, y, 500, 24},
                   NULL, vm->progress_text,
                   &vm->model.progress, 0.0f, 1.0f);

    y += 40;

    GuiLabel((Rectangle){20, y, 100, 20}, "Status:");
    GuiLabel((Rectangle){120, y, 500, 20}, vm->model.status_message);

    y += 35;

    GuiLabel((Rectangle){20, y, 200, 20}, "Received files:");
    y += 25;

    for (int i = 0; i < vm->model.received_count && i < 8; i++) {
        char line[320];
        snprintf(line, sizeof(line), "%s  (%llu bytes)",
                 vm->model.received[i].name,
                 (unsigned long long)vm->model.received[i].size);
        GuiLabel((Rectangle){20, y, 600, 20}, line);
        y += 22;
    }
}

