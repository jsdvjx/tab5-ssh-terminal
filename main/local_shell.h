// Local esp_console shell rendered into the on-screen terminal.
// Ctrl+Alt+T toggles between the SSH session and the shell: while active the
// keyboard sink points here (bytes arrive already terminal-encoded, i.e.
// downstream of the power-mgmt and IME hooks) and command output goes through
// term_feed(). Type `help` for the command list.
#pragma once

#include "term.h"
#include "settings.h"

// Register commands, start the shell task and hook Ctrl+Alt+T. Call after
// ime_filter_init() (the `ime` command) and before ssh_client_start().
// `s` must stay valid forever (the `sleep`/`target` commands touch it).
void local_shell_init(term_t *t, settings_t *s);

// Enter the shell programmatically (e.g. when no SSH targets exist).
void local_shell_enter(void);
void local_shell_leave(void);   // an SSH session takes the keyboard
bool local_shell_active(void);
