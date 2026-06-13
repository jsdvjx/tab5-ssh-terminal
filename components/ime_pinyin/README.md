# ime_pinyin — Pinyin IME decoder component (libgooglepinyin port)

Reusable ESP-IDF component wrapping the AOSP PinyinIME decoding engine for the
M5Stack Tab5 (ESP32-P4) SSH terminal's Chinese input method.

## Origin & license
- Upstream: https://github.com/qgears/libgooglepinyin
  (CMake-ified extraction of the AOSP PinyinIME engine),
  commit `6055722c98bf203ecde8e109a0d36c8a6a2e8007`.
- License: **Apache-2.0** (AOSP, Copyright 2009 The Android Open Source
  Project). Original headers retained in every source file.
- All local modifications are marked with `// tab5 port:` comments.
- The pristine clone, dictbuilder data and host test harness live in
  `tools/ime_host/`. This component is the single source of truth for the
  engine sources; the host Makefile compiles from here.

## Changes vs upstream
1. **Portable dictionary format ("v2 uint32")** — upstream serialized raw
   `size_t` fields into `dict_pinyin.dat` (8 bytes on a 64-bit host, 4 on the
   32-bit P4). All `fread`/`fwrite` of size counters in `dicttrie.cpp`,
   `dictlist.cpp`, `ngram.cpp`, `spellingtrie.cpp` now use
   `dict_size_t` (= `uint32_t`, see `dictdef.h`). `LmaNodeLE0` — written raw
   with `fwrite(root_, sizeof(LmaNodeLE0), ...)` — also had two `size_t`
   members converted to `uint32` (it was 24 bytes on the 64-bit builder host
   vs 16 on riscv32, corrupting the trie + overflowing `splid_le0_index_` on
   target load). **Old .dat files no longer load — rebuild with
   `tools/ime_host` (`make`) or `tools/make_assets.py pinyin`.**
   New dict: 1,068,442 bytes.
2. **Memory-source loading** — `im_open_decoder_mem(buf, len, fn_usr_dict)`
   opens the decoder from a dictionary image already in RAM (e.g. PSRAM,
   loaded from SD by assets.c). Implemented with `fmemopen()` (present in
   ESP-IDF newlib *and* the P4 picolibc, verified in the toolchain libc.a).
   The buffer may be freed after the call; everything is copied to heap.
3. **PSRAM allocator shim** (`include/ime_alloc.h`) — all large dictionary
   buffers (dicttrie nodes/index, dictlist, ngram codebook, spelling trie
   buffer, matrixsearch share pool) go through `ime_malloc`/`ime_free`:
   `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with malloc
   fallback on ESP, plain malloc on host.
4. **Builder code excluded on target** — `___BUILD_MODEL___` in `dictdef.h`
   is now gated behind `IME_PINYIN_BUILD_MODEL` (defined only by the host
   dictbuilder build). `dictbuilder.cpp`, `spellingtable.cpp`,
   `utf16reader.cpp` are kept in `src/` for the host build but not compiled
   into the component.
5. **Bug/portability fixes** — `DictTrie::free_resource` freed `nodes_ge1_`
   twice and leaked `lma_idx_buf_`; mixed `new[]`/`free()` pairs unified onto
   `ime_malloc`/`ime_free`; `UserDict::fuzzy_compare_spell_id` defined as
   `int` but declared `int32` (build error on riscv32 where `int32_t` is
   `long`); `%d`-with-`size_t` printf warnings fixed with casts.

## API
Include `ime_pinyin.h` (re-exports upstream `pinyinime.h` + the `_mem`
opener). The API lives in C++ namespace `ime_pinyin`; call from a .cpp file.

```cpp
#include "ime_pinyin.h"
using namespace ime_pinyin;

// dict already loaded into PSRAM (e.g. by assets.c from SD)
im_open_decoder_mem(dict_buf, dict_len, NULL /* no user dict */);
im_set_max_lens(32, 16);

size_t n = im_search("nihao", 5);          // n candidates
char16 buf[17];                            // UTF-16LE output
im_get_candidate(0, buf, 16);              // -> 你好
im_choose(0);                              // fix candidate / build sentence
im_reset_search();
im_close_decoder();
```

User dictionary: pass a VFS path (e.g. `"/sdcard/usr_dict.dat"`) instead of
NULL. userdict/sync use pthread mutexes (ESP-IDF pthread is in REQUIRES);
this path is compiled in but not yet exercised on target — treat as TODO.

## Memory expectations
- ~1.5–2 MB heap allocated through `ime_malloc` (PSRAM) for the decoded
  dictionary structures + search pools, held while the decoder is open.
- Plus the 1.07 MB raw dict buffer you pass to `im_open_decoder_mem`
  (only needed during the call — can be freed right after it returns).
- Host-measured peak RSS for open+search: ~2.8 MB.

## Testing
Host regression (clang, compiles *these* sources):

```
cd tools/ime_host && make test
```

Validates nihao→你好, zhongguo→中国, zhg abbreviation, and
woshizhongguoren→我是中国人 via both `im_open_decoder` (file) and
`im_open_decoder_mem` (in-memory) paths. Sources also pass a
`riscv32-esp-elf-g++ -std=gnu++14 -Wall -Wformat -fsyntax-only` check.
