# libgooglepinyin host build (macOS) — groundwork for Tab5 ESP-IDF IME port

> **UPDATE (port done):** the engine now lives in `components/ime_pinyin/`
> (ported, `// tab5 port:` changes: v2 uint32 dict format, im_open_decoder_mem,
> PSRAM alloc shim). This Makefile now compiles the COMPONENT sources, not the
> pristine `libgooglepinyin/` clone (kept for reference + dictbuilder main +
> raw dict data). `make test` exercises file and in-memory open paths.
> The dict here is the **v2 uint32 format incl. fixed-width LmaNodeLE0 (1,068,442 bytes)** — old-format
> notes below are historical.

## Source
- Repo: https://github.com/qgears/libgooglepinyin (CMake-ified extraction of the
  AOSP PinyinIME engine, Apache-2.0), commit `6055722c98bf203ecde8e109a0d36c8a6a2e8007`.
- Includes raw dictionary data (`data/rawdict_utf16_65105_freq.txt`, 65101 lemmas,
  `data/valid_utf16.txt`) and the dict builder (`tools/pinyinime_dictbuilder.cpp`).

## Build (cmake is not installed on this machine; plain Makefile used)
```
cd tools/ime_host
make            # builds lib, dictbuilder, dict_pinyin.dat, test_cli
make test
```
Compiles cleanly with Apple clang, `-std=c++14`. **Zero source changes required**
on macOS. Only warnings: 5x `-Wunused-but-set-variable`, 4x `-Wformat`
(`%d` used with `size_t` — must be fixed for the ESP build, where size_t is 32-bit).

## Dictionary
`build/dict_pinyin.dat` = **1,071,858 bytes (~1.05 MB)**.

IMPORTANT: the .dat file format embeds raw `size_t` fields (e.g.
`fread(&lma_node_num_le0_, sizeof(size_t), 1, fp)` in dicttrie.cpp/spellingtrie.cpp/
ngram.cpp/dictlist.cpp). A .dat built on 64-bit macOS is NOT loadable on 32-bit
ESP32-P4 as-is. For the port: change on-disk fields to `uint32_t`, or build the dict
with a 32-bit-size_t builder. (This 1.07 MB file happens to load on the host; sizes
written as 8-byte size_t.)

## Test results (all pass)
- `nihao` → cand0 你好 (then 你 拟 尼 呢 泥 妳 妮 腻 逆)
- `zhongguo` → cand0 中国
- `zhg` / `zh'g` (abbreviation) → 这个, 中国, 整个, 找个, 照顾, 职工, 主管, 中共, 正规, 珍贵 (1150 cands)
- `woshizhongguoren` --choose → 我是中国人 (cand0 was already the full Viterbi
  sentence; one `im_choose(0)` fixes it, im_choose returns 1 ⇒ done)

## File I/O & RAM behavior (key for ESP port)
`im_open_decoder` → `DictTrie::load_dict(filename,...)`:
- Opens the file once, sequentially `fread`s **everything** into heap-allocated
  buffers (SpellingTrie, DictList, DictTrie nodes/idx, NGram codebook), then
  `fclose(fp)`. **No FILE* kept, no lazy reads, no mmap, no seeks after load**
  (only `im_open_decoder_fd` does one initial fseek to start_offset).
- Runtime search allocates fixed pools at load: `parsing_marks_` (kMaxParsingMark
  ParsingMark) and `mile_stones_` (kMaxMileStone MileStone) in dicttrie.cpp.
- Measured host peak RSS for open+search: ~2.8 MB total ⇒ in-RAM dict structures
  ≈ 1.5–2 MB. Perfect fit for Tab5 32 MB PSRAM.
- UserDict (`/tmp/usr_dict.dat`) is separate, small, optional.

## ESP-IDF porting notes / required changes
1. Replace `FILE*`-based loaders with memory reads, or simpler: keep fopen/fread —
   ESP-IDF VFS supports it — and load the .dat from SD; everything ends up in heap
   anyway. To force PSRAM, swap `malloc` → `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
   in dicttrie.cpp, dictlist.cpp, ngram.cpp, spellingtrie.cpp (one small shim header).
2. Fix `size_t` in the .dat format (uint32) or generate the dict with 4-byte size_t.
3. Fix the `-Wformat` printf warnings (debug printfs only) and disable
   ___BUILD_MODEL___ in dictdef.h on target (drops dictbuilder code).
4. `sync.cpp`/`userdict.cpp` use pthread mutex — available in ESP-IDF, or stub out.

## Files
- `Makefile`, `test_cli.cpp` (CLI: `./test_cli dict.dat <pinyin> [--choose]`)
- `libgooglepinyin/` — pristine upstream clone, unmodified
- `build/` — artifacts (libgooglepinyin.a, pinyinime_dictbuilder, dict_pinyin.dat, test_cli)
