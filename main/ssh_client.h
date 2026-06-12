#pragma once

#include <stddef.h>
#include <stdint.h>
#include "term.h"
#include "settings.h"

// Called whenever the active target changes (after a switch request).
typedef void (*ssh_target_changed_cb_t)(int index);

// Starts the SSH task connecting to s->targets[s->last_target].
// `s` must stay valid for the lifetime of the task.
void ssh_client_start(term_t *t, settings_t *s, ssh_target_changed_cb_t cb);

// Queue bytes for the remote side (keyboard input, terminal responses).
void ssh_client_send(const uint8_t *data, size_t len);

// Ask the session task to drop the current connection and connect to
// targets[index]. Safe from any task (including LVGL callbacks).
void ssh_client_request_switch(int index);

// Tell the remote PTY that the terminal size changed (panel toggled).
void ssh_client_request_resize(void);
