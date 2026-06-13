// CLI test for libgooglepinyin host build.
// Usage: ./test_cli <dict_pinyin.dat> <pinyin> [--choose] [--mem]
//   default: print top-10 candidates for the pinyin string
//   --choose: repeatedly im_choose(0) to build the full sentence
//   --mem: slurp the dict into RAM and open via im_open_decoder_mem
//          (tab5 port: exercises the PSRAM/in-memory loading path)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "ime_pinyin.h"

using namespace ime_pinyin;

static std::string utf16_to_utf8(const char16 *s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) {
      out += (char)(0xC0 | (c >> 6));
      out += (char)(0x80 | (c & 0x3F));
    } else {
      out += (char)(0xE0 | (c >> 12));
      out += (char)(0x80 | ((c >> 6) & 0x3F));
      out += (char)(0x80 | (c & 0x3F));
    }
  }
  return out;
}

static std::string cand(size_t id) {
  char16 buf[64];
  if (!im_get_candidate(id, buf, 63)) return "";
  size_t n = 0; while (n < 63 && buf[n]) n++;
  return utf16_to_utf8(buf, n);
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s dict.dat pinyin [--choose] [--mem]\n", argv[0]); return 1; }
  bool choose = false, mem = false, inc = false;
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "--choose")) choose = true;
    if (!strcmp(argv[i], "--mem")) mem = true;
    if (!strcmp(argv[i], "--inc")) inc = true;   // tab5 port: per-keystroke path
  }
  unsigned char *dictbuf = NULL;
  if (mem) {
    // tab5 port: emulate the firmware path (dict already in PSRAM)
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
    dictbuf = (unsigned char*)malloc(len);
    if (fread(dictbuf, 1, len, fp) != (size_t)len) { fclose(fp); return 1; }
    fclose(fp);
    if (!im_open_decoder_mem(dictbuf, (size_t)len, NULL)) {
      fprintf(stderr, "im_open_decoder_mem failed\n"); return 1;
    }
    printf("opened via im_open_decoder_mem (%ld bytes)\n", len);
  } else if (!im_open_decoder(argv[1], "/tmp/usr_dict.dat")) {
    fprintf(stderr, "im_open_decoder failed\n"); return 1;
  }
  im_set_max_lens(32, 16);
  const char *py = argv[2];
  if (inc) {
    // tab5 port: emulate per-keystroke input the way the firmware does
    // (im_add_letter is an unimplemented stub upstream — it returns 0 —
    // so the filter must re-im_search the growing buffer each key).
    im_reset_search();
    bool ok = true;
    for (size_t l = 1; l <= strlen(py); l++) {
      size_t n = im_search(py, l);
      printf("inc %.*s -> cands=%zu cand0=%s\n", (int)l, py, n, cand(0).c_str());
      if (n == 0) ok = false;
    }
    if (!ok) { fprintf(stderr, "FAIL: zero candidates mid-composition\n"); return 1; }
    im_close_decoder();
    free(dictbuf);
    return 0;
  }
  size_t ncand = im_search(py, strlen(py));
  size_t dlen = 0;
  printf("input=%s decoded=%s cands=%zu\n", py, im_get_sps_str(&dlen), ncand);
  if (choose) {
    // candidate 0 is the current best sentence; im_choose fixes it piecewise.
    // When the whole input is fixed, im_choose returns 1 (only the full
    // sentence remains as a candidate).
    size_t n = ncand;
    while (n > 1) {
      printf("choose(0), fixed_len=%zu, cand0=%s\n", im_get_fixed_len(),
             cand(0).c_str());
      n = im_choose(0);
      if (n == 0) break;
    }
    printf("sentence=%s\n", cand(0).c_str());
  } else {
    for (size_t i = 0; i < ncand && i < 10; i++)
      printf("%zu: %s\n", i, cand(i).c_str());
  }
  im_close_decoder();
  free(dictbuf);
  return 0;
}
