#include "assets.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "assets";

#define ASSET_DIR "/sdcard/tab5"
#define EMOJI_PX  24
#define EMOJI_BYTES (EMOJI_PX * EMOJI_PX * 4)

static lv_font_t *s_nerd;
static lv_font_t *s_cjk;
static uint8_t *s_pinyin_dict;
static size_t s_pinyin_dict_len;

typedef struct {
    uint32_t cp;
    uint32_t off;
} emoji_idx_t;

static int s_emoji_count;
static emoji_idx_t *s_emoji_idx;
static uint8_t *s_emoji_blobs;
static lv_image_dsc_t *s_emoji_dscs;   // one per emoji, persistent for draw tasks

// OS / brand icon atlases (OSI1). One descriptor per target_os_t id, indexed
// directly by id (ids are small and contiguous 0..TARGET_OS_COUNT-1).
#define OSICON_MAX_ID 16
typedef struct {
    int px;                                    // 32 or 64
    int count;
    lv_image_dsc_t *by_id[OSICON_MAX_ID];      // NULL if id absent
    uint8_t *buf;                              // owns the slurped atlas
} osicon_atlas_t;

static osicon_atlas_t s_os64;
static osicon_atlas_t s_os32;

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    *out_len = rd;
    return buf;
}

static lv_font_t *load_binfont(const char *path)
{
    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) return NULL;
    lv_font_t *font = lv_binfont_create_from_buffer(buf, len);
    // NOTE: lv_binfont keeps internal copies of tables it needs; the loader
    // documentation is ambiguous, so the buffer is intentionally kept
    // resident in PSRAM (we have 32MB; correctness over kilobytes).
    if (!font) {
        ESP_LOGW(TAG, "binfont load failed: %s", path);
        free(buf);
    }
    return font;
}

static void load_emoji(const char *path)
{
    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) return;
    if (len < 8 || memcmp(buf, "EMJ1", 4) != 0) {
        ESP_LOGW(TAG, "bad emoji atlas header");
        free(buf);
        return;
    }
    uint32_t count;
    memcpy(&count, buf + 4, 4);
    size_t idx_bytes = (size_t)count * sizeof(emoji_idx_t);
    if (len < 8 + idx_bytes + (size_t)count * EMOJI_BYTES) {
        ESP_LOGW(TAG, "emoji atlas truncated");
        free(buf);
        return;
    }
    s_emoji_idx = (emoji_idx_t *)(buf + 8);
    s_emoji_blobs = buf + 8 + idx_bytes;
    s_emoji_count = count;

    s_emoji_dscs = heap_caps_calloc(count, sizeof(lv_image_dsc_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (uint32_t i = 0; i < count; i++) {
        lv_image_dsc_t *d = &s_emoji_dscs[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_ARGB8888;
        d->header.w = EMOJI_PX;
        d->header.h = EMOJI_PX;
        d->header.stride = EMOJI_PX * 4;
        d->data_size = EMOJI_BYTES;
        d->data = s_emoji_blobs + s_emoji_idx[i].off;
    }
    ESP_LOGI(TAG, "emoji atlas: %d glyphs", s_emoji_count);
}

static void load_osicons(const char *path, osicon_atlas_t *at, int px)
{
    size_t len;
    uint8_t *buf = slurp(path, &len);
    if (!buf) return;
    const size_t blob = (size_t)px * px * 4;
    if (len < 8 || memcmp(buf, "OSI1", 4) != 0) {
        ESP_LOGW(TAG, "bad osicon atlas header: %s", path);
        free(buf);
        return;
    }
    uint32_t count;
    memcpy(&count, buf + 4, 4);
    size_t idx_bytes = (size_t)count * sizeof(emoji_idx_t);   // {u32 id, u32 off}
    if (len < 8 + idx_bytes + (size_t)count * blob) {
        ESP_LOGW(TAG, "osicon atlas truncated: %s", path);
        free(buf);
        return;
    }
    emoji_idx_t *idx = (emoji_idx_t *)(buf + 8);
    uint8_t *blobs = buf + 8 + idx_bytes;

    // Descriptors live at the tail of the same PSRAM buffer is awkward; keep a
    // small static array per atlas inside the struct via a calloc.
    lv_image_dsc_t *dscs = heap_caps_calloc(count, sizeof(lv_image_dsc_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dscs) { free(buf); return; }
    at->px = px;
    at->count = count;
    at->buf = buf;
    for (uint32_t i = 0; i < count; i++) {
        lv_image_dsc_t *d = &dscs[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_ARGB8888;
        d->header.w = px;
        d->header.h = px;
        d->header.stride = px * 4;
        d->data_size = blob;
        d->data = blobs + idx[i].off;
        uint32_t id = idx[i].cp;   // {cp,off} struct reused: cp field holds id
        if (id < OSICON_MAX_ID) at->by_id[id] = d;
    }
    ESP_LOGI(TAG, "os icons: %d @%d", at->count, px);
}

void assets_load(void)
{
    if (bsp_sdcard_mount() != ESP_OK) {
        ESP_LOGI(TAG, "no SD card - using built-in fonts only");
        return;
    }

    s_nerd = load_binfont(ASSET_DIR "/nerd24.bin");
    s_cjk = load_binfont(ASSET_DIR "/cjkfull24.bin");
    load_emoji(ASSET_DIR "/emoji24.bin");
    load_osicons(ASSET_DIR "/osicons64.bin", &s_os64, 64);
    load_osicons(ASSET_DIR "/osicons32.bin", &s_os32, 32);
    s_pinyin_dict = slurp(ASSET_DIR "/dict_pinyin.dat", &s_pinyin_dict_len);

    bsp_sdcard_unmount();
    ESP_LOGI(TAG, "assets: nerd=%s cjkfull=%s emoji=%d osicons=%d@64/%d@32 pinyin=%u",
             s_nerd ? "yes" : "no", s_cjk ? "yes" : "no", s_emoji_count,
             s_os64.count, s_os32.count, (unsigned)s_pinyin_dict_len);
}

const uint8_t *assets_pinyin_dict(size_t *len)
{
    if (len) *len = s_pinyin_dict_len;
    return s_pinyin_dict;
}

const lv_font_t *assets_font_nerd(void) { return s_nerd; }
const lv_font_t *assets_font_cjk(void)  { return s_cjk; }

const lv_image_dsc_t *assets_os_icon(int os_id, int size)
{
    const osicon_atlas_t *at = (size >= 64) ? &s_os64 : &s_os32;
    if (!at->buf || os_id < 0 || os_id >= OSICON_MAX_ID) return NULL;
    return at->by_id[os_id];   // NULL if this id wasn't in the atlas
}

const lv_image_dsc_t *assets_emoji(uint32_t cp)
{
    int lo = 0, hi = s_emoji_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_emoji_idx[mid].cp == cp) return &s_emoji_dscs[mid];
        if (s_emoji_idx[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}
