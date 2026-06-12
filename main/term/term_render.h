// LVGL renderer for the terminal grid. Owns a full-screen canvas and a
// periodic timer that redraws dirty rows only.
#pragma once

#include "term.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call after bsp_display_start(). Takes the LVGL lock internally.
void term_render_start(term_t *t);

// Raw canvas pixels for the web screenshot endpoint (RGB565).
// Returns NULL before term_render_start.
const uint8_t *term_render_framebuffer(int *w, int *h, int *stride_bytes);

#ifdef __cplusplus
}
#endif
