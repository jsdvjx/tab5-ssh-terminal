/*
 * tab5 port: public header for the ime_pinyin component.
 *
 * Re-exports the upstream libgooglepinyin im_* decoder API (pinyinime.h)
 * plus the tab5 additions. NOTE: despite the extern "C" block in
 * pinyinime.h, the API lives in the C++ namespace ime_pinyin, so this
 * header is usable from C++ translation units (LVGL UI code compiled as
 * C++ or via a thin wrapper).
 *
 * Typical use on target:
 *   const asset_t *dict = assets_get("dict_pinyin.dat");  // PSRAM buffer
 *   ime_pinyin::im_open_decoder_mem(dict->data, dict->size, NULL);
 *   ime_pinyin::im_search("nihao", 5);
 *   ime_pinyin::im_get_candidate(0, buf16, 16);  // -> UTF-16LE "你好"
 */
#ifndef IME_PINYIN_IME_PINYIN_H__
#define IME_PINYIN_IME_PINYIN_H__

#include <stddef.h>
#include "pinyinime.h"   /* upstream im_* API (namespace ime_pinyin) */
#include "ime_alloc.h"   /* ime_malloc/ime_free shim */

#ifdef __cplusplus
extern "C" {
#endif

namespace ime_pinyin {

/**
 * tab5 port: open the decoder engine from an in-memory system-dictionary
 * image (e.g. dict_pinyin.dat already loaded into PSRAM from SD).
 *
 * The buffer must stay valid only for the duration of this call; all data
 * is copied into heap structures allocated via ime_malloc (PSRAM-first on
 * ESP). Requires the "v2 uint32" dictionary format produced by the patched
 * dictbuilder in tools/ime_host.
 *
 * @param sys_dict_buf  Pointer to the raw dict_pinyin.dat image.
 * @param len           Size of the image in bytes.
 * @param fn_usr_dict   User-dictionary file path, or NULL to disable the
 *                      user dictionary.
 * @return true on success.
 */
bool im_open_decoder_mem(const unsigned char *sys_dict_buf, size_t len,
                         const char *fn_usr_dict);

}  // namespace ime_pinyin

#ifdef __cplusplus
}
#endif

#endif  // IME_PINYIN_IME_PINYIN_H__
