#pragma once

#include <stddef.h>
#include <stdint.h>
#include "term.h"
#include "settings.h"

// Concurrent SSH sessions ("tabs"). Each open session owns a term_t, a
// libssh2 session and a FreeRTOS task; only the ACTIVE session's terminal is
// rendered, the others keep feeding their grids in the background.
// Hard cap: libssh2/mbedTLS keep their crypto state in internal RAM
// (~10-15KB per session) plus a 6KB task stack, so four is plenty.
#define MAX_SSH_SESSIONS 4

// Connection state, per session. `target`/`cap` receive the session target's
// display name (pass NULL to skip). Safe from any task.
typedef enum {
    SSH_STATE_IDLE,         // no targets / slot free
    SSH_STATE_CONNECTING,
    SSH_STATE_CONNECTED,
} ssh_state_t;

// Called whenever the active session's target changes (open/switch).
typedef void (*ssh_target_changed_cb_t)(int index);

// Starts the session machinery. No session is opened: the UI (home panel
// SSH app), Ctrl+Alt+1..9 or the local shell open them. `t` becomes session
// 0's terminal; `s` must stay valid forever.
void ssh_client_start(term_t *t, settings_t *s, ssh_target_changed_cb_t cb);

// Open a session to targets[target_idx] and make it active. If a session to
// that target is already open it is reused (just switched to). Returns the
// session id, or -1 (bad index, MAX_SSH_SESSIONS reached, or internal heap
// too low — a message lands on the active terminal).
int ssh_open(int target_idx);

// Ask session `id` to disconnect and free its slot (async: the session task
// finishes its current blocking call first). If it was active, the next open
// session becomes active. Returns the number of sessions left open.
int ssh_close(int id);

// Queue bytes for session `id` (keyboard input, terminal responses).
void ssh_send(int id, const uint8_t *data, size_t len);

// Active-session shim: keyboard sink / IME commit / legacy callers.
void ssh_client_send(const uint8_t *data, size_t len);

// Active session id (-1 if none open) / switch the screen to session `id`.
int  ssh_active(void);
bool ssh_set_active(int id);

// The terminal the user is looking at: active session's term_t, or the boot
// terminal when nothing is open (local shell etc.). NULL before
// ssh_client_start() — callers fall back to their own term.
term_t *ssh_active_term(void);

// Per-session state; id outside [0,MAX_SSH_SESSIONS) or a free slot returns
// SSH_STATE_IDLE. `target_idx` (optional) receives the target index.
ssh_state_t ssh_state(int id, char *target, size_t cap, int *target_idx);

// Number of open sessions.
int ssh_session_count(void);

// Open-or-switch by TARGET index (panel tap / web config / `target n` /
// Ctrl+Alt+1..9). Reuses an existing session to that target, else opens a
// new one. Safe from any task (including LVGL callbacks).
void ssh_client_request_switch(int index);

// Active session's state for the status bar (legacy signature).
ssh_state_t ssh_client_get_state(char *target, size_t cap);

// Tell every open session's PTY that the terminal size changed.
void ssh_client_request_resize(void);
