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
