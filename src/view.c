#include "view.h"
#include "raygui.h"
#include "raylib.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

void view_draw(ViewModel *vm) {
  int y = 20;

  GuiLabel((Rectangle){20, y, 200, 20}, "Target IP:");

  if (GuiTextBox((Rectangle){20, y + 25, 200, 30}, vm->model.target_ip,
                 MAX_IP_LEN, vm->ip_edit_mode)) {
    vm->ip_edit_mode = !vm->ip_edit_mode;
  }

  GuiLabel((Rectangle){240, y, 200, 20}, "File path:");

  /* File path validation. */
  bool file_ok = false;
  if (vm->model.file_path[0] != '\0') {
    struct stat st;
    file_ok = (stat(vm->model.file_path, &st) == 0 && S_ISREG(st.st_mode));
  }

  int saved_text_color = GuiGetStyle(TEXTBOX, TEXT_COLOR_NORMAL);
  if (vm->model.file_path[0] != '\0' && !file_ok) {
    GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, 0xC83232FF);
  }

  bool was_editing_path = vm->path_edit_mode;
  if (GuiTextBox((Rectangle){240, y + 25, 380, 30}, vm->model.file_path,
                 MAX_PATH_LEN, vm->path_edit_mode)) {
    vm->path_edit_mode = !vm->path_edit_mode;
  }

  GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL, saved_text_color);

  /* Enter on the path box commits and triggers send. */
  if (was_editing_path && !vm->path_edit_mode && IsKeyPressed(KEY_ENTER) &&
      file_ok) {
    vm->path_committed_with_enter = true;
  }

  y += 70;

  /* Send button (enabled only when file is valid). */
  bool can_send = vm->send_enabled && file_ok;
  if (!can_send) {
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, 0xCCCCCCFF);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, 0x888888FF);
  }
  bool send_clicked = GuiButton((Rectangle){20, y, 100, 30}, "Send") ||
                      vm->path_committed_with_enter;
  GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, 0xF5F5F5FF);
  GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, 0x2E2E2EFF);

  vm->path_committed_with_enter = false;

  if (send_clicked && can_send) {
    vm_start_send(vm);
  }

  /* Cancel button (enabled only during an active transfer). */
  if (!vm->cancel_enabled) {
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, 0xCCCCCCFF);
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, 0x888888FF);
  }
  bool cancel_clicked = GuiButton((Rectangle){130, y, 100, 30}, "Cancel");
  GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, 0xF5F5F5FF);
  GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, 0x2E2E2EFF);

  if (cancel_clicked && vm->cancel_enabled) {
    vm_cancel(vm);
  }

  y += 45;

  GuiLabel((Rectangle){20, y, 100, 20}, "Progress:");
  GuiProgressBar((Rectangle){120, y, 460, 24}, NULL, NULL, &vm->model.progress,
                 0.0f, 1.0f);
  GuiLabel((Rectangle){590, y, 50, 24}, vm->progress_text);

  y += 30;

  if (vm->transport) {
    char bbr_line[256];
    snprintf(bbr_line, sizeof(bbr_line),
             "BBR: %s   pacing: %llu B/s   cwnd: %u packets",
             transport_get_bbr_state(vm->transport),
             (unsigned long long)transport_get_pacing_rate(vm->transport),
             transport_get_cwnd(vm->transport));
    GuiLabel((Rectangle){20, y, 600, 20}, bbr_line);
  } else {
    GuiLabel((Rectangle){20, y, 600, 20}, "BBR: (idle)");
  }

  y += 25;

  GuiLabel((Rectangle){20, y, 100, 20}, "Status:");
  GuiLabel((Rectangle){120, y, 500, 20}, vm->model.status_message);

  y += 35;

  GuiLabel((Rectangle){20, y, 200, 20}, "Received files:");
  y += 25;

  for (int i = 0; i < vm->model.received_count && i < 8; i++) {
    char line[320];
    snprintf(line, sizeof(line), "%s  (%llu bytes)", vm->model.received[i].name,
             (unsigned long long)vm->model.received[i].size);
    GuiLabel((Rectangle){20, y, 600, 20}, line);
    y += 22;
  }
}
