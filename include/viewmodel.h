#ifndef VIEWMODEL_H
#define VIEWMODEL_H

#include <pthread.h>
#include "model.h"
#include "transport.h" // Ensure Transport is defined

typedef struct
{
  TransferModel model;

  char progress_text[64];
  char state_text[64];
  bool send_enabled;
  bool cancel_enabled;

  bool ip_edit_mode;
  bool path_edit_mode;

  Transport *transport;

  /* NEW: Threading */
  pthread_t poll_thread;
  bool thread_running;
} ViewModel;

void vm_init(ViewModel *vm);
void vm_update(ViewModel *vm);
void vm_start_send(ViewModel *vm);
void vm_cancel(ViewModel *vm);

#endif

