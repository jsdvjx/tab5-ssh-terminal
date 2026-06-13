// Pinyin IME key filter: sits between hid_send_key() and the terminal/SSH
// sink. Ctrl+Space toggles Chinese input; while composing it owns the
// keyboard, drives the candidate bar (ime_bar.c) and commits UTF-8 to
// ssh_client_send(). Engine: components/ime_pinyin (libgooglepinyin).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "term.h"

#ifdef __cplusplus
extern "C" {
#endif

// Open the decoder from the SD dictionary (assets_pinyin_dict). Call after
// assets_load(). Without a dictionary the IME stays unavailable and
// ime_filter_handle_key() never consumes anything. `t` is used for the
// bracketed-paste query on commit.
void ime_filter_init(term_t *t);

// Gate: only the SSH phase enables the filter (setup wants raw keys).
void ime_filter_set_enabled(bool enabled);

// Called at the top of hid_send_key(). Returns true if the key was consumed
// by the IME (toggle, composition editing, candidate pick...).
bool ime_filter_handle_key(uint8_t usage, uint8_t modifiers);

// Send arbitrary UTF-8 to the SSH session, wrapped in bracketed-paste
// markers when the remote app asked for them. Used by voice input.
void ime_filter_commit_text(const char *text);

// Touch callbacks from the candidate bar (run in the LVGL task).
void ime_filter_pick(int visible_idx);      // 0..6 within the current page
void ime_filter_page(int dir);              // -1 prev page, +1 next page

// Debug helper for GET /api/ime: run a throwaway search for `pinyin` and
// return up to max_cands UTF-8 candidates (each NUL-terminated, cap 64).
// Returns the candidate count, or -1 if the engine is unavailable.
int ime_filter_debug_query(const char *pinyin, char cands[][64], int max_cands);

#ifdef __cplusplus
}
#endif
