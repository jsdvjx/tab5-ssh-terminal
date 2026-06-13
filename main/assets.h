// SD-card asset pack: optional richer fonts/emoji loaded into PSRAM at boot.
// Files live in /sdcard/tab5/ (see tools/make_assets.py). The SD card is
// mounted, slurped and unmounted BEFORE Wi-Fi starts — the C6 shares the
// SDMMC peripheral and concurrent use is known-flaky.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

// Mount SD, load whatever assets exist, unmount. Safe without a card.
void assets_load(void);

// NULL until the corresponding asset was loaded.
const lv_font_t *assets_font_nerd(void);    // PUA symbols (powerline etc.)
const lv_font_t *assets_font_cjk(void);     // full CJK (replaces flash GB2312)

// 24x24 ARGB8888 emoji image for a codepoint, NULL if not in atlas.
const lv_image_dsc_t *assets_emoji(uint32_t cp);

// Colored OS / brand icon for an SSH target, ARGB8888. `size` is 32 or 64
// (anything >=64 picks the 64px atlas, else 32px). Returns NULL if the
// osicons atlas wasn't loaded or the id has no icon — callers must fall back
// to the Nerd Font glyph. `os_id` is a target_os_t (settings.h):
//   0 SERVER (generic synthesized rack)  1 APPLE   2 TUX (linux)  3 UBUNTU
//   4 DEBIAN   5 WINDOWS (windows11)   6 RASPBERRY
// Source SVGs are Devicon "-original" colored variants (MIT); id 0 is a
// synthesized slate/teal server-rack glyph. See tools/make_assets.py.
const lv_image_dsc_t *assets_os_icon(int os_id, int size);

// Raw pinyin IME dictionary (dict_pinyin.dat) in PSRAM, NULL if absent.
// ime_filter copies what it needs at init; the buffer stays resident.
const uint8_t *assets_pinyin_dict(size_t *len);
