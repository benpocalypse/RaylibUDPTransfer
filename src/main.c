#include "raylib.h"
#include "viewmodel.h"
#include "view.h"
#include "transport.h"      /* NEW: brings in TransportCallbacks, Transport */
#include <string.h>         /* NEW: strcmp */
#include <unistd.h>         /* NEW: usleep */
#include <stdio.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

/* Headless receive mode callback. */
static void on_file_received_headless(const char *name, uint64_t size, void *user) {
    (void)user;
    printf("Received file: %s (%llu bytes)\n",
           name, (unsigned long long)size);
}

static int run_receive_mode(void) {
    TransportCallbacks cbs = {0};
    cbs.on_file_received = on_file_received_headless;

    /* Header declares: transport_create(host, port, cbs, user)
     * The host here is the local bind address; the transport uses
     * whatever peer the tunnel forwards to it. */
    Transport *t = transport_create("0.0.0.0", 3000, &cbs, NULL);
    if (!t) {
        fprintf(stderr, "Failed to create transport\n");
        return 1;
    }

    for (;;) {
        transport_poll(t);
        usleep(1000);
    }

    transport_destroy(t);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--receive") == 0) {
        return run_receive_mode();
    }

    const int screen_w = 640;
    const int screen_h = 480;

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

