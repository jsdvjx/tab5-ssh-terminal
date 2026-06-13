/*
 * tab5 port: allocation shim for libgooglepinyin.
 *
 * The decoder loads ~1.5-2 MB of dictionary structures to heap. On the
 * ESP32-P4 target these must land in PSRAM, not internal SRAM, so every
 * large buffer in dicttrie/dictlist/ngram/spellingtrie/matrixsearch is
 * routed through ime_malloc()/ime_free().
 *
 * - ESP_PLATFORM: heap_caps_malloc(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),
 *   falling back to plain malloc if PSRAM allocation fails.
 * - Host (macOS/Linux): plain malloc/free.
 */
#ifndef IME_PINYIN_IME_ALLOC_H__
#define IME_PINYIN_IME_ALLOC_H__

#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

static inline void *ime_malloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL)
    p = malloc(size);
  return p;
}

static inline void ime_free(void *p) {
  free(p);  // heap_caps allocations are free()-compatible in ESP-IDF
}

#else  // host build

static inline void *ime_malloc(size_t size) {
  return malloc(size);
}

static inline void ime_free(void *p) {
  free(p);
}

#endif  // ESP_PLATFORM

#endif  // IME_PINYIN_IME_ALLOC_H__
