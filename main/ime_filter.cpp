// Pinyin IME state machine. C++ TU because the libgooglepinyin im_* API
// lives in the C++ namespace ime_pinyin; the exported API (ime_filter.h)
// is plain C.
//
// Threads: hid_send_key() runs on the USB HID callback and the I2C keyboard
// poll task; ime_filter_pick()/_page() run on the LVGL task; the /api/ime
// debug endpoint runs on the httpd task. The engine is not thread-safe, so
// every im_* call happens under s_mu.
//
// LOCK ORDERING: s_mu (engine) is taken for engine work only and is RELEASED
// before calling ime_bar_show()/ime_bar_hide() (which take bsp_display_lock)
// or ssh_client_send(). ime_bar.c never holds any lock of its own when its
// touch callbacks call back into ime_filter_pick()/_page(). This keeps
// s_mu and the LVGL lock from ever being taken in opposite orders.

#include "ime_filter.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "ime_pinyin.h"
#include "ime_bar.h"
extern "C" {                    // C headers without their own extern "C" guards
#include "assets.h"
#include "ssh_client.h"
#include "status_bar.h"
}

using namespace ime_pinyin;

static const char *TAG = "ime";

// HID usage ids (boot keyboard page; see hid_keyboard.c keymap)
#define K_A      0x04
#define K_Z      0x1d
#define K_1      0x1e
#define K_9      0x26
#define K_ENTER  0x28
#define K_ESC    0x29
#define K_BKSP   0x2a
#define K_SPACE  0x2c
#define K_MINUS  0x2d
#define K_EQUAL  0x2e
#define K_QUOTE  0x34   // apostrophe: pinyin syllable separator
#define K_RIGHT  0x4f
#define K_LEFT   0x50

#define MOD_CTRL  0x11
#define MOD_SHIFT 0x22
#define MOD_ALT   0x44

#define PY_MAX      32
#define CACHE_MAX   32
#define CAND_UTF8   64

static term_t *s_term;
static SemaphoreHandle_t s_mu;
static bool s_avail;                 // decoder opened
static volatile bool s_enabled;     // SSH phase only
static bool s_on;                   // Ctrl+Space toggle
static bool s_composing;

static char s_py[PY_MAX + 1];       // our copy of the typed pinyin
static size_t s_py_len;

static char s_cands[CACHE_MAX][CAND_UTF8];  // UTF-8 candidate cache
static int s_cand_count;            // cached entries (<= CACHE_MAX)
static size_t s_total;              // engine's full candidate count
static int s_page;                  // index of first visible candidate
static char s_sps[CAND_UTF8];       // spelling string shown left of the cands

// Snapshot of the visible page, filled under s_mu, displayed after release.
typedef struct {
    char sps[CAND_UTF8];
    char cands[IME_BAR_CANDS][CAND_UTF8];
    int n;
    bool has_prev, has_next;
} page_snap_t;

// ------------------------------------------------------------- utf16 -> utf8

static size_t utf16_to_utf8(const char16 *u, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; u[i]; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xd800 && cp <= 0xdbff && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (u[i + 1] - 0xdc00);
            i++;
        }
        if (cp < 0x80) {
            if (o + 1 >= cap) break;
            out[o++] = cp;
        } else if (cp < 0x800) {
            if (o + 2 >= cap) break;
            out[o++] = 0xc0 | (cp >> 6);
            out[o++] = 0x80 | (cp & 0x3f);
        } else if (cp < 0x10000) {
            if (o + 3 >= cap) break;
            out[o++] = 0xe0 | (cp >> 12);
            out[o++] = 0x80 | ((cp >> 6) & 0x3f);
            out[o++] = 0x80 | (cp & 0x3f);
        } else {
            if (o + 4 >= cap) break;
            out[o++] = 0xf0 | (cp >> 18);
            out[o++] = 0x80 | ((cp >> 12) & 0x3f);
            out[o++] = 0x80 | ((cp >> 6) & 0x3f);
            out[o++] = 0x80 | (cp & 0x3f);
        }
    }
    out[o] = 0;
    return o;
}

// --------------------------------------------------------- cache & snapshot

// Refill the candidate cache after a search/choose. Called under s_mu.
static void refresh_cache_locked(size_t total)
{
    s_total = total;
    s_cand_count = total < CACHE_MAX ? (int)total : CACHE_MAX;
    for (int i = 0; i < s_cand_count; i++) {
        char16 buf[32];
        if (im_get_candidate(i, buf, 31)) {
            utf16_to_utf8(buf, s_cands[i], CAND_UTF8);
        } else {
            s_cands[i][0] = 0;
        }
    }
    size_t decoded;
    const char *sps = im_get_sps_str(&decoded);
    strlcpy(s_sps, sps ? sps : "", sizeof(s_sps));
    if (s_page >= s_cand_count) s_page = 0;
}

// Copy the visible page out of the cache. Called under s_mu.
static void snap_locked(page_snap_t *snap)
{
    strlcpy(snap->sps, s_sps, sizeof(snap->sps));
    snap->n = s_cand_count - s_page;
    if (snap->n > IME_BAR_CANDS) snap->n = IME_BAR_CANDS;
    if (snap->n < 0) snap->n = 0;
    for (int i = 0; i < snap->n; i++) {
        strlcpy(snap->cands[i], s_cands[s_page + i], CAND_UTF8);
    }
    snap->has_prev = s_page > 0;
    snap->has_next = (size_t)(s_page + IME_BAR_CANDS) < s_total;
}

static void show_snap(const page_snap_t *snap)
{
    const char *ptrs[IME_BAR_CANDS];
    for (int i = 0; i < snap->n; i++) ptrs[i] = snap->cands[i];
    ime_bar_show(snap->sps, ptrs, snap->n, snap->has_prev, snap->has_next);
}

// ------------------------------------------------------------------- commit

static void commit_utf8(const char *text)
{
    size_t len = strlen(text);
    if (!len) return;
    term_t *t = ssh_active_term();      // bracketed paste is per session
    if (!t) t = s_term;
    if (t && term_bracketed_paste(t)) {
        uint8_t buf[6 + sizeof(s_cands[0]) * 2 + 6];
        size_t o = 0;
        memcpy(buf + o, "\x1b[200~", 6); o += 6;
        if (len > sizeof(buf) - 12) len = sizeof(buf) - 12;
        memcpy(buf + o, text, len); o += len;
        memcpy(buf + o, "\x1b[201~", 6); o += 6;
        ssh_client_send(buf, o);
    } else {
        ssh_client_send((const uint8_t *)text, len);
    }
}

// Public commit for non-IME text sources (voice input). Unlike commit_utf8
// the text can be arbitrarily long, so the bracketed-paste markers go out as
// separate sends instead of one stack buffer. Doesn't touch the engine — no
// s_mu needed.
void ime_filter_commit_text(const char *text)
{
    size_t len = strlen(text);
    if (!len) return;
    term_t *t = ssh_active_term();
    if (!t) t = s_term;
    if (t && term_bracketed_paste(t)) {
        ssh_client_send((const uint8_t *)"\x1b[200~", 6);
        ssh_client_send((const uint8_t *)text, len);
        ssh_client_send((const uint8_t *)"\x1b[201~", 6);
    } else {
        ssh_client_send((const uint8_t *)text, len);
    }
}

// Exit composition. Called under s_mu.
static void reset_locked(void)
{
    im_reset_search();
    s_composing = false;
    s_py_len = 0;
    s_py[0] = 0;
    s_page = 0;
}

// Pick candidate `id` (absolute engine id). Called under s_mu; on full
// resolution copies the commit text to `commit` (cap CAND_UTF8) and returns
// true (caller commits after releasing s_mu), else refreshes the cache and
// returns false (caller shows the snapshot).
static bool choose_locked(size_t id, char *commit, page_snap_t *snap)
{
    size_t r = im_choose(id);
    if (r == 1) {
        // Whole input resolved: candidate 0 is the fixed sentence.
        char16 buf[64];
        commit[0] = 0;
        if (im_get_candidate(0, buf, 63)) {
            utf16_to_utf8(buf, commit, CAND_UTF8);
        }
        reset_locked();
        return true;
    }
    s_page = 0;
    refresh_cache_locked(r);
    snap_locked(snap);
    return false;
}

// ---------------------------------------------------------------- public API

void ime_filter_init(term_t *t)
{
    s_term = t;
    size_t len = 0;
    const uint8_t *dict = assets_pinyin_dict(&len);
    if (!dict || !len) {
        ESP_LOGW(TAG, "no dict_pinyin.dat on SD - pinyin IME unavailable");
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    if (!im_open_decoder_mem(dict, len, NULL)) {
        ESP_LOGW(TAG, "decoder open failed - pinyin IME unavailable");
        return;
    }
    im_set_max_lens(PY_MAX, 16);
    s_avail = true;
    ESP_LOGI(TAG, "pinyin decoder ready (%u byte dict)", (unsigned)len);
}

void ime_filter_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool ime_filter_handle_key(uint8_t usage, uint8_t modifiers)
{
    if (!s_avail || !s_enabled) return false;

    bool ctrl = modifiers & MOD_CTRL;
    bool alt  = modifiers & MOD_ALT;

    if (ctrl && !alt && usage == K_SPACE) {        // toggle IME
        bool was_composing;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_on = !s_on;
        was_composing = s_composing;
        if (!s_on && s_composing) reset_locked();
        xSemaphoreGive(s_mu);
        if (was_composing && !s_on) ime_bar_hide();
        status_bar_set_ime(s_on ? "中" : "EN");
        return true;
    }
    if (!s_on) return false;

    page_snap_t snap;
    char commit[CAND_UTF8];

    xSemaphoreTake(s_mu, portMAX_DELAY);

    if (!s_composing) {
        if (!ctrl && !alt && usage >= K_A && usage <= K_Z) {
            s_composing = true;
            s_py_len = 0;
            s_page = 0;
            im_reset_search();
            char ch = 'a' + (usage - K_A);
            s_py[s_py_len++] = ch;
            s_py[s_py_len] = 0;
            // im_add_letter() is an unimplemented stub upstream (returns 0);
            // always search the authoritative buffer instead.
            refresh_cache_locked(im_search(s_py, s_py_len));
            snap_locked(&snap);
            xSemaphoreGive(s_mu);
            show_snap(&snap);
            return true;
        }
        xSemaphoreGive(s_mu);
        return false;                               // everything else: raw
    }

    // ---- composing: the IME owns the keyboard until cancel/commit ----

    if (!ctrl && !alt && ((usage >= K_A && usage <= K_Z) || usage == K_QUOTE)) {
        if (s_py_len < PY_MAX) {
            char ch = (usage == K_QUOTE) ? '\'' : 'a' + (usage - K_A);
            s_py[s_py_len++] = ch;
            s_py[s_py_len] = 0;
            s_page = 0;
            // See above: im_add_letter is a stub; im_search reuses the
            // engine's prefix milestones, so this stays incremental.
            refresh_cache_locked(im_search(s_py, s_py_len));
        }
        snap_locked(&snap);
        xSemaphoreGive(s_mu);
        show_snap(&snap);
        return true;
    }

    switch (usage) {
    case K_BKSP:
        s_py[--s_py_len] = 0;
        if (s_py_len == 0) {
            reset_locked();
            xSemaphoreGive(s_mu);
            ime_bar_hide();
            return true;
        }
        // Re-search the whole buffer (drops any partially fixed choices —
        // good enough, and keeps our buffer authoritative).
        im_reset_search();
        s_page = 0;
        refresh_cache_locked(im_search(s_py, s_py_len));
        snap_locked(&snap);
        xSemaphoreGive(s_mu);
        show_snap(&snap);
        return true;

    case K_ESC:
        im_cancel_input();
        reset_locked();
        xSemaphoreGive(s_mu);
        ime_bar_hide();
        return true;

    case K_ENTER: {                                 // commit the raw pinyin
        char raw[PY_MAX + 1];
        size_t raw_len = s_py_len;
        memcpy(raw, s_py, s_py_len + 1);
        reset_locked();
        xSemaphoreGive(s_mu);
        ime_bar_hide();
        ssh_client_send((const uint8_t *)raw, raw_len);
        return true;
    }

    case K_LEFT:
    case K_MINUS:
    case K_RIGHT:
    case K_EQUAL: {
        int dir = (usage == K_RIGHT || usage == K_EQUAL) ? 1 : -1;
        int page = s_page + dir * IME_BAR_CANDS;
        if (page >= 0 && page < s_cand_count) s_page = page;
        snap_locked(&snap);
        xSemaphoreGive(s_mu);
        show_snap(&snap);
        return true;
    }

    default:
        break;
    }

    int pick = -1;                                  // visible index to choose
    if (usage >= K_1 && usage <= K_9) pick = usage - K_1;
    else if (usage == K_SPACE) pick = 0;

    if (pick >= 0 && s_page + pick < s_cand_count && pick < IME_BAR_CANDS) {
        if (choose_locked(s_page + pick, commit, &snap)) {
            xSemaphoreGive(s_mu);
            ime_bar_hide();
            commit_utf8(commit);
        } else {
            xSemaphoreGive(s_mu);
            show_snap(&snap);
        }
        return true;
    }

    // Anything else (arrows up/down, tab...) is swallowed while composing so
    // the remote app never sees half an input session.
    xSemaphoreGive(s_mu);
    return true;
}

// Touch pick from the candidate bar (LVGL task; we hold no locks on entry).
void ime_filter_pick(int visible_idx)
{
    if (!s_avail) return;
    page_snap_t snap;
    char commit[CAND_UTF8];

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s_composing || visible_idx < 0 || visible_idx >= IME_BAR_CANDS
        || s_page + visible_idx >= s_cand_count) {
        xSemaphoreGive(s_mu);
        return;
    }
    if (choose_locked(s_page + visible_idx, commit, &snap)) {
        xSemaphoreGive(s_mu);
        ime_bar_hide();
        commit_utf8(commit);
    } else {
        xSemaphoreGive(s_mu);
        show_snap(&snap);
    }
}

void ime_filter_page(int dir)
{
    if (!s_avail) return;
    page_snap_t snap;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!s_composing) {
        xSemaphoreGive(s_mu);
        return;
    }
    int page = s_page + (dir > 0 ? IME_BAR_CANDS : -IME_BAR_CANDS);
    if (page >= 0 && page < s_cand_count) s_page = page;
    snap_locked(&snap);
    xSemaphoreGive(s_mu);
    show_snap(&snap);
}

int ime_filter_debug_query(const char *pinyin, char cands[][64], int max_cands)
{
    if (!s_avail) return -1;
    size_t len = strlen(pinyin);
    if (len > PY_MAX) len = PY_MAX;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    im_reset_search();
    size_t total = im_search(pinyin, len);
    int n = (int)total < max_cands ? (int)total : max_cands;
    for (int i = 0; i < n; i++) {
        char16 buf[32];
        if (im_get_candidate(i, buf, 31)) utf16_to_utf8(buf, cands[i], 64);
        else cands[i][0] = 0;
    }
    im_reset_search();
    // Restore an interrupted live composition (best effort).
    if (s_composing && s_py_len) im_search(s_py, s_py_len);
    xSemaphoreGive(s_mu);
    return n;
}
