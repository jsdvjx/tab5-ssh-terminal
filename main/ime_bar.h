// Candidate bar for the pinyin IME: full-width strip at the bottom of the
// screen showing the spelling string + up to 7 tappable candidates.
// All calls are safe from any task (bsp_display_lock inside).
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IME_BAR_CANDS 7

// Create the bar (hidden). Call once after status_bar_init().
void ime_bar_create(void);

// Show/refresh: `pinyin` is the spelling string, `cands_utf8` the visible
// page (n <= IME_BAR_CANDS). Strings are copied into LVGL labels; the caller
// may free them right after the call.
void ime_bar_show(const char *pinyin, const char *const cands_utf8[], int n,
                  bool has_prev, bool has_next);

void ime_bar_hide(void);

#ifdef __cplusplus
}
#endif
