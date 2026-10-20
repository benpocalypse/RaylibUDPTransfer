#include "raylib.h"
#include "viewmodel.h"
#include "view.h"
#include "transport.h"
#include "unit_tests.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

/* Headless receive mode callback. */
static void on_file_received_headless(const char *name, uint64_t size, void *user)
{
  (void)user;
  printf("Received file: %s (%llu bytes)\n",
         name, (unsigned long long)size);
}

static int run_receive_mode(void)
{
  TransportCallbacks cbs = {0};
  cbs.on_file_received = on_file_received_headless;

  /* 4-argument signature: bind address, then peer address.
   * In receive mode the peer address is a placeholder — the
   * receiver learns the sender's address from the first packet
   * it receives. */
  Transport *t = transport_create("0.0.0.0", 3000,
                                  "127.0.0.1", 0,
                                  &cbs, NULL);
  if (!t)
  {
      fprintf(stderr, "Failed to create transport\n");
      return 1;
  }

  fprintf(stderr, "Receiver listening on 0.0.0.0:3000\n");

  for (;;)
  {
      transport_poll(t);
      usleep(1000);
  }

  transport_destroy(t);

  return 0;
}

int main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "--run-unit-tests") == 0)
  {
    return run_unit_tests();
  }

  if (argc > 1 && strcmp(argv[1], "--receive") == 0)
  {
      return run_receive_mode();
  }

  fprintf(stderr, "main: argc=%d argv[1]=%s\n", argc, argc > 1 ? argv[1] : "(none)");

  const int screen_w = 640;
  const int screen_h = 480;

  InitWindow(screen_w, screen_h, "UDP File Transfer");

  Font font = LoadFontEx("resources/Inconsolata-Regular.ttf", 64, NULL, 0);
  GuiSetFont(font);
  GuiSetStyle(DEFAULT, TEXT_SIZE, 16);

  SetTargetFPS(60);

  ViewModel vm;
  vm_init(&vm);

  while (!WindowShouldClose())
  {
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

