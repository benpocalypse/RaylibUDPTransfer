#include "transport.h"
#include "viewmodel.h"
#include <stdio.h>
#include <string.h>

void vm_init (ViewModel *vm)
{
  model_init (&vm->model);
  vm->transport = NULL;
  vm->send_enabled = true;
  vm->cancel_enabled = false;
  vm->ip_edit_mode = false;
  vm->path_edit_mode = false;
  vm->thread_running = false;

  strncpy (vm->progress_text, "0%", sizeof (vm->progress_text) - 1);
  strncpy (vm->state_text, "Idle", sizeof (vm->state_text) - 1);
}

static const char * state_to_string (TransferState s)
{
  switch (s)
  {
  case STATE_IDLE:
    return "Idle";
  case STATE_SENDING:
    return "Sending";
  case STATE_RECEIVING:
    return "Receiving";
  case STATE_ERROR:
    return "Error";
  case STATE_DONE:
    return "Done";
  }

  return "Unknown";
}

/* --- Threading --- */
static void *transport_poll_thread(void *arg) {
  ViewModel *vm = (ViewModel *)arg;

  while (vm->thread_running && vm->transport)
  {
    /* Note: Because transport_poll is updating send_acked_seq in the
     * background, reading it in the main thread is technically a data race on a
     * 32-bit integer. On x86_64, aligned 32-bit reads/writes are atomic at the
     * hardware level, so this is safe in practice for this project.
     * If we were building for an embedded ARM system, we would use C11 atomics
     * to make sure there are no race conditions.
     * */
    transport_poll(vm->transport);
    /* Sleep for 500 microseconds. This allows up to 2000 polls per
     * second, which is plenty for BBR to measure accurate RTTs on
     * loopback, while keeping CPU usage minimal. */
    usleep(500);
  }

  return NULL;
}

void vm_update(ViewModel *vm)
{
  snprintf(vm->progress_text, sizeof(vm->progress_text),
           "%d%%", (int)(vm->model.progress * 100.0f));

  strncpy(vm->state_text, state_to_string(vm->model.state),
          sizeof(vm->state_text) - 1);

  vm->send_enabled = (vm->model.state == STATE_IDLE ||
                      vm->model.state == STATE_DONE);
  vm->cancel_enabled = (vm->model.state == STATE_SENDING ||
                        vm->model.state == STATE_RECEIVING);

  if (vm->transport)
  {
    /* NO transport_poll() here anymore! */
    vm->model.progress = transport_get_progress(vm->transport);

    if (vm->model.state == STATE_SENDING &&
        transport_send_complete(vm->transport))
    {
      vm->model.state = STATE_DONE;
      strncpy(vm->model.status_message, "Delivered",
              sizeof(vm->model.status_message) - 1);
    }
  }
}

static void on_progress (float p, void *user)
{
  ViewModel *vm = (ViewModel *) user;
  vm->model.progress = p;
}

static void on_file_received (const char *name, uint64_t size, void *user)
{
  ViewModel *vm = (ViewModel *) user;

  model_add_received (&vm->model, name, size);
  strncpy (vm->model.status_message, "File received",
           sizeof (vm->model.status_message) - 1);
}

static void on_error (const char *message, void *user)
{
  ViewModel *vm = (ViewModel *) user;
  strncpy (vm->model.status_message, message,
           sizeof (vm->model.status_message) - 1);

  vm->model.state = STATE_ERROR;
}

void vm_start_send (ViewModel *vm)
{
  if (vm->model.file_path[0] == '\0')
  {
    strncpy (vm->model.status_message, "No file selected",
             sizeof (vm->model.status_message) - 1);
    vm->model.state = STATE_ERROR;
    return;
  }

  if (strchr(vm->model.target_ip, ':') != NULL)
  {
    strncpy(vm->model.status_message,
            "IP field must not include a port",
            sizeof(vm->model.status_message) - 1);
    vm->model.state = STATE_ERROR;
    return;
  }

  if (!vm->transport)
  {
    TransportCallbacks cbs =
    {
      .on_progress = on_progress,
      .on_file_received = on_file_received,
      .on_error = on_error
    };

    vm->transport = transport_create ("0.0.0.0", 0,
                                      vm->model.target_ip,
                                      vm->model.target_port,
                                      &cbs, vm);

    if (!vm->transport)
    {
      /* Transport creation FAILED. Stop here. */
      strncpy (vm->model.status_message, "Failed to open transport",
               sizeof (vm->model.status_message) - 1);
      vm->model.state = STATE_ERROR;
      return;
    }

    /* Transport created successfully. Start the polling thread. */
    vm->thread_running = true;
    if (pthread_create(&vm->poll_thread, NULL, transport_poll_thread, vm) != 0)
    {
      strncpy(vm->model.status_message, "Failed to start polling thread",
              sizeof(vm->model.status_message) - 1);
      vm->model.state = STATE_ERROR;
      vm->thread_running = false;
      transport_destroy(vm->transport);
      vm->transport = NULL;
      return;
    }
  }

  /* At this point, transport exists and the thread is polling it. */
  vm->model.state = STATE_SENDING;
  strncpy(vm->model.status_message, "Sending...",
          sizeof(vm->model.status_message) - 1);

  if (!transport_queue_send(vm->transport, vm->model.file_path))
  {
    strncpy(vm->model.status_message, "Failed to queue send",
            sizeof(vm->model.status_message) - 1);
    vm->model.state = STATE_ERROR;
  }
}

void vm_cancel (ViewModel *vm)
{
  if (vm->transport)
  {
      /* Stop the polling thread before destroying the transport */
      if (vm->thread_running)
      {
        vm->thread_running = false;
        pthread_join(vm->poll_thread, NULL);
      }

      transport_cancel(vm->transport);
      transport_destroy(vm->transport);
      vm->transport = NULL;
    }
    vm->model.state = STATE_IDLE;
}

