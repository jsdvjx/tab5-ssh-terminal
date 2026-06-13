// Headless VT100/xterm terminal emulator core: byte stream in, cell grid out.
// No rendering here — term_render.c draws the grid with LVGL.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

// Grid geometry follows the font chosen in menuconfig (1280x720 panel).
#if CONFIG_TAB5_FONT_SMALL          // unscii-16
#define TERM_CELL_W 8
#define TERM_CELL_H 16
#elif CONFIG_TAB5_FONT_LARGE        // JetBrains Mono 30 (advance 18, line 32)
#define TERM_CELL_W 18
#define TERM_CELL_H 36
#else                               // JetBrains Mono 20 (advance 12, line 21)
#define TERM_CELL_W 12
#define TERM_CELL_H 24
#endif

// The right third of the 1280px screen is the status/quick-switch panel;
// the terminal gets the rest, and can expand to the full width when the
// panel is hidden (Ctrl+Alt+P). The grid is allocated at TERM_COLS_MAX and
// the active width switches at runtime (term_set_cols + PTY resize).
#define TAB5_PANEL_W 424
#define TERM_COLS_MAX   (1280 / TERM_CELL_W)
#define TERM_COLS_PANEL ((1280 - TAB5_PANEL_W) / TERM_CELL_W)
// The top of the screen hosts the status bar (status_bar.c): reserve at
// least 24px, give the bar whatever the cell grid can't use anyway.
#define TERM_ROWS ((720 - 24) / TERM_CELL_H)
#define TAB5_STATUS_BAR_H (720 - TERM_ROWS * TERM_CELL_H)

typedef struct {
    uint32_t ch;        // unicode codepoint (rendered subset: ASCII + CP437-ish)
    uint32_t fg;        // 0xRRGGBB
    uint32_t bg;        // 0xRRGGBB
    uint8_t  attrs;     // TERM_ATTR_*
} term_cell_t;

#define TERM_ATTR_BOLD      (1 << 0)
#define TERM_ATTR_UNDERLINE (1 << 1)
#define TERM_ATTR_REVERSE   (1 << 2)

typedef struct term term_t;

// Called when the emulator must answer the host (DSR, DA, etc.).
// Wire this to ssh_client_send().
typedef void (*term_response_cb_t)(const uint8_t *data, size_t len, void *ctx);

term_t *term_create(void);
void term_set_response_cb(term_t *t, term_response_cb_t cb, void *ctx);

// Feed raw bytes from the SSH channel. Thread-safe (takes internal lock).
void term_feed(term_t *t, const uint8_t *data, size_t len);

// Display width of a codepoint in cells (1, or 2 for CJK/fullwidth/emoji).
// Continuation cells of a wide char have ch == 0 in the grid.
int term_cp_width(uint32_t cp);

// Active column count (TERM_COLS_PANEL or TERM_COLS_MAX).
int  term_cols(term_t *t);
// Switch width; clears the screen (the remote app repaints on SIGWINCH).
void term_set_cols(term_t *t, int cols);

// Mark everything dirty (e.g. after fonts become available).
void term_redraw(term_t *t);

// Renderer access. Hold the lock while reading the grid.
void term_lock(term_t *t);
void term_unlock(term_t *t);
const term_cell_t *term_row(term_t *t, int row);   // TERM_COLS_MAX cells (stride)
bool term_row_dirty(term_t *t, int row);
void term_clear_dirty(term_t *t);                  // call after a full redraw pass
void term_cursor(term_t *t, int *col, int *row, bool *visible);

// True while the host app (vim, claude /tui fullscreen) is on the alt screen.
bool term_alt_screen(term_t *t);

// True while the host app enabled bracketed paste (DECSET 2004). IME commits
// wrap multi-byte text in ESC[200~ .. ESC[201~ when this is on.
bool term_bracketed_paste(term_t *t);

// Keyboard helpers: arrow keys honor DECCKM (application cursor keys mode).
// Returns escape sequence for the current mode into buf, returns length.
typedef enum { TERM_KEY_UP, TERM_KEY_DOWN, TERM_KEY_RIGHT, TERM_KEY_LEFT,
               TERM_KEY_HOME, TERM_KEY_END, TERM_KEY_PGUP, TERM_KEY_PGDN,
               TERM_KEY_DELETE, TERM_KEY_INSERT } term_key_t;
int term_encode_key(term_t *t, term_key_t key, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
