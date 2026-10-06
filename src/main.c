#include "raylib.h"
#include "viewmodel.h"
#include "view.h"

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

int main(int argc, char **argv) {
    const int screen_w = 640;
    const int screen_h = 480;
    bool receive_mode = false;
  
    if (argc > 1 && strcmp(argv[1], "--receive") == 0) {
        receive_mode = true;
    }

    if (receive_mode) {
        TransportCallbacks cbs = {0};
        cbs.on_file_received = on_file_received_headless;
        Transport *t = transport_create("0.0.0.0", 3000, &cbs, NULL);
        while (1) {
            transport_poll(t);
            usleep(1000);  // 1mS - don't hog the CPU
        }
    }

    InitWindow(screen_w, screen_h, "UDP File Transfer");
  
    // Load the default font at 2x size for crisp rendering on scaled displays
    Font font = LoadFontEx("resources/Inconsolata-Regular.ttf", 64, NULL, 0);
    GuiSetFont(font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 32);  // 32px logical, rendered from 64px source
  
    SetTargetFPS(60);

    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);

    ViewModel vm;
    vm_init(&vm);

    while (!WindowShouldClose()) {
        vm_update(&vm);

        BeginDrawing();
        ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));
        view_draw(&vm);
        EndDrawing();
    }

    vm_cancel(&vm);
    CloseWindow();
    return 0;
}

