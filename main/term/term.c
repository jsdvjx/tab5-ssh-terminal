// Minimal xterm-256color emulator core. Covers what Claude Code, tmux and
// common shells actually emit; rarer sequences are parsed and dropped, with a
// debug log so coverage gaps show up in `idf.py monitor` instead of as
// garbage on screen.

#include "term.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "term";
volatile uint32_t g_dbg_term_feed;

#define DEFAULT_FG 0xd0d0d0
#define DEFAULT_BG 0x101010

#define MAX_PARAMS 16

typedef enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_CHARSET } parse_state_t;

struct term {
    term_cell_t *grid;        // active grid (points at main or alt)
    int cols;                 // active width (stride is TERM_COLS_MAX)
    term_cell_t *main_grid;
    term_cell_t *alt_grid;
    bool alt_active;

    int cur_col, cur_row;
    int saved_col, saved_row;
    bool cursor_visible;
    bool app_cursor_keys;     // DECCKM
    int scroll_top, scroll_bot;   // inclusive, 0-based

    uint32_t fg, bg;
    uint8_t attrs;

    bool dirty[TERM_ROWS];

    // parser
    parse_state_t state;
    int params[MAX_PARAMS];
    int nparams;
    bool csi_private;
    char osc_buf[64];
    int osc_len;

    // utf-8 decode
    uint32_t utf_cp;
    int utf_left;

    term_response_cb_t resp_cb;
    void *resp_ctx;

    SemaphoreHandle_t mutex;
};

// ---------------------------------------------------------------- palette

static const uint32_t ansi16[16] = {
    0x000000, 0xcc0000, 0x4e9a06, 0xc4a000, 0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf,
    0x555753, 0xef2929, 0x8ae234, 0xfce94f, 0x729fcf, 0xad7fa8, 0x34e2e2, 0xeeeeec,
};

static uint32_t color256(int idx)
{
    if (idx < 16) return ansi16[idx];
    if (idx < 232) {            // 6x6x6 cube
        int c = idx - 16;
        static const uint8_t lvl[6] = { 0, 95, 135, 175, 215, 255 };
        return (lvl[c / 36] << 16) | (lvl[(c / 6) % 6] << 8) | lvl[c % 6];
    }
    int g = 8 + (idx - 232) * 10;   // grayscale ramp
    return (g << 16) | (g << 8) | g;
}

// ---------------------------------------------------------------- grid ops

static inline term_cell_t *cell(term_t *t, int col, int row)
{
    return &t->grid[row * TERM_COLS_MAX + col];
}

static void blank_cell(term_t *t, term_cell_t *c)
{
    c->ch = ' ';
    c->fg = t->fg;
    c->bg = t->bg;
    c->attrs = 0;
}

static void clear_region(term_t *t, int c0, int r0, int c1, int r1)
{
    for (int r = r0; r <= r1; r++) {
        for (int c = (r == r0 ? c0 : 0); c <= (r == r1 ? c1 : t->cols - 1); c++) {
            blank_cell(t, cell(t, c, r));
        }
        t->dirty[r] = true;
    }
}

static void scroll_up(term_t *t, int top, int bot, int n)
{
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(&t->grid[top * TERM_COLS_MAX],
            &t->grid[(top + n) * TERM_COLS_MAX],
            (bot - top + 1 - n) * TERM_COLS_MAX * sizeof(term_cell_t));
    clear_region(t, 0, bot - n + 1, t->cols - 1, bot);
    for (int r = top; r <= bot; r++) t->dirty[r] = true;
}

static void scroll_down(term_t *t, int top, int bot, int n)
{
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(&t->grid[(top + n) * TERM_COLS_MAX],
            &t->grid[top * TERM_COLS_MAX],
            (bot - top + 1 - n) * TERM_COLS_MAX * sizeof(term_cell_t));
    clear_region(t, 0, top, t->cols - 1, top + n - 1);
    for (int r = top; r <= bot; r++) t->dirty[r] = true;
}

static void all_dirty(term_t *t)
{
    for (int r = 0; r < TERM_ROWS; r++) t->dirty[r] = true;
}

// ---------------------------------------------------------------- cursor

static void clamp_cursor(term_t *t)
{
    if (t->cur_col < 0) t->cur_col = 0;
    if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
    if (t->cur_row < 0) t->cur_row = 0;
    if (t->cur_row >= TERM_ROWS) t->cur_row = TERM_ROWS - 1;
}

static void linefeed(term_t *t)
{
    if (t->cur_row == t->scroll_bot) {
        scroll_up(t, t->scroll_top, t->scroll_bot, 1);
    } else {
        t->cur_row++;
        clamp_cursor(t);
    }
}

int term_cp_width(uint32_t cp)
{
    if (cp < 0x1100) return 1;
    if ((cp >= 0x1100 && cp <= 0x115F) ||      // hangul jamo
        (cp >= 0x2E80 && cp <= 0x303E) ||      // CJK radicals, punctuation
        (cp >= 0x3041 && cp <= 0x33FF) ||      // kana, CJK symbols
        (cp >= 0x3400 && cp <= 0x4DBF) ||      // CJK ext A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||      // CJK unified
        (cp >= 0xA000 && cp <= 0xA4CF) ||      // yi
        (cp >= 0xAC00 && cp <= 0xD7A3) ||      // hangul syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||      // CJK compat
        (cp >= 0xFE30 && cp <= 0xFE4F) ||      // CJK compat forms
        (cp >= 0xFF00 && cp <= 0xFF60) ||      // fullwidth forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||    // emoji
        (cp >= 0x20000 && cp <= 0x3FFFD)) {    // CJK ext B+
        return 2;
    }
    return 1;
}

static void put_char(term_t *t, uint32_t cp)
{
    // Zero-width: ZWSP/ZWNJ/ZWJ, variation selectors, combining marks.
    // Emoji variation sequences (e.g. 2764 FE0F) keep just the base char.
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x0300 && cp <= 0x036F)) {
        return;
    }
    int w = term_cp_width(cp);
    if (t->cur_col + w > t->cols) {     // wrap (covers deferred wrap too)
        t->cur_col = 0;
        linefeed(t);
    }
    term_cell_t *c = cell(t, t->cur_col, t->cur_row);
    c->ch = cp;
    c->fg = (t->attrs & TERM_ATTR_REVERSE) ? t->bg : t->fg;
    c->bg = (t->attrs & TERM_ATTR_REVERSE) ? t->fg : t->bg;
    c->attrs = t->attrs;
    if (w == 2) {                       // continuation cell
        term_cell_t *c2 = cell(t, t->cur_col + 1, t->cur_row);
        c2->ch = 0;
        c2->fg = c->fg;
        c2->bg = c->bg;
        c2->attrs = c->attrs;
    }
    t->dirty[t->cur_row] = true;
    t->cur_col += w;                    // may reach t->cols: wrap on next char
}

// ---------------------------------------------------------------- CSI

static int param(term_t *t, int i, int dflt)
{
    if (i >= t->nparams || t->params[i] <= 0) return dflt;
    return t->params[i];
}

static void respond(term_t *t, const char *s)
{
    if (t->resp_cb) t->resp_cb((const uint8_t *)s, strlen(s), t->resp_ctx);
}

static void do_sgr(term_t *t)
{
    if (t->nparams == 0) { t->nparams = 1; t->params[0] = 0; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i] < 0 ? 0 : t->params[i];
        switch (p) {
        case 0:  t->fg = DEFAULT_FG; t->bg = DEFAULT_BG; t->attrs = 0; break;
        case 1:  t->attrs |= TERM_ATTR_BOLD; break;
        case 4:  t->attrs |= TERM_ATTR_UNDERLINE; break;
        case 7:  t->attrs |= TERM_ATTR_REVERSE; break;
        case 22: t->attrs &= ~TERM_ATTR_BOLD; break;
        case 24: t->attrs &= ~TERM_ATTR_UNDERLINE; break;
        case 27: t->attrs &= ~TERM_ATTR_REVERSE; break;
        case 39: t->fg = DEFAULT_FG; break;
        case 49: t->bg = DEFAULT_BG; break;
        case 38: case 48: {
            uint32_t col = 0;
            if (param(t, i + 1, 0) == 5) {              // 256-color
                col = color256(param(t, i + 2, 0));
                i += 2;
            } else if (param(t, i + 1, 0) == 2) {       // truecolor
                col = (param(t, i + 2, 0) << 16) | (param(t, i + 3, 0) << 8)
                      | param(t, i + 4, 0);
                i += 4;
            }
            if (p == 38) t->fg = col; else t->bg = col;
            break;
        }
        default:
            if (p >= 30 && p <= 37)        t->fg = ansi16[p - 30];
            else if (p >= 40 && p <= 47)   t->bg = ansi16[p - 40];
            else if (p >= 90 && p <= 97)   t->fg = ansi16[p - 90 + 8];
            else if (p >= 100 && p <= 107) t->bg = ansi16[p - 100 + 8];
            else ESP_LOGD(TAG, "SGR %d ignored", p);
        }
    }
}

static void enter_alt(term_t *t, bool alt)
{
    if (alt == t->alt_active) return;
    t->alt_active = alt;
    t->grid = alt ? t->alt_grid : t->main_grid;
    if (alt) clear_region(t, 0, 0, t->cols - 1, TERM_ROWS - 1);
    all_dirty(t);
}

static void do_mode(term_t *t, bool set)
{
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (!t->csi_private) { ESP_LOGD(TAG, "mode %d ignored", p); continue; }
        switch (p) {
        case 1:    t->app_cursor_keys = set; break;       // DECCKM
        case 25:   t->cursor_visible = set; break;
        case 47: case 1047:
            enter_alt(t, set); break;
        case 1049:                                        // alt screen + save cursor
            if (set) { t->saved_col = t->cur_col; t->saved_row = t->cur_row; }
            enter_alt(t, set);
            if (!set) { t->cur_col = t->saved_col; t->cur_row = t->saved_row; }
            break;
        case 2004:                                        // bracketed paste
        case 1000: case 1002: case 1003: case 1006:       // mouse reporting
            break;                                        // accepted, unimplemented
        default:
            ESP_LOGD(TAG, "DEC mode ?%d %s ignored", p, set ? "h" : "l");
        }
    }
}

static void do_csi(term_t *t, char final)
{
    int n = param(t, 0, 1);
    switch (final) {
    case 'A': t->cur_row -= n; clamp_cursor(t); break;
    case 'B': t->cur_row += n; clamp_cursor(t); break;
    case 'C': t->cur_col += n; clamp_cursor(t); break;
    case 'D': t->cur_col -= n; clamp_cursor(t); break;
    case 'E': t->cur_col = 0; t->cur_row += n; clamp_cursor(t); break;
    case 'F': t->cur_col = 0; t->cur_row -= n; clamp_cursor(t); break;
    case 'G': t->cur_col = param(t, 0, 1) - 1; clamp_cursor(t); break;
    case 'd': t->cur_row = param(t, 0, 1) - 1; clamp_cursor(t); break;
    case 'H': case 'f':
        t->cur_row = param(t, 0, 1) - 1;
        t->cur_col = param(t, 1, 1) - 1;
        clamp_cursor(t);
        break;
    case 'J': {
        int p = param(t, 0, 0);
        if (p == 0)      clear_region(t, t->cur_col, t->cur_row, t->cols - 1, TERM_ROWS - 1);
        else if (p == 1) clear_region(t, 0, 0, t->cur_col, t->cur_row);
        else             clear_region(t, 0, 0, t->cols - 1, TERM_ROWS - 1);
        break;
    }
    case 'K': {
        int p = param(t, 0, 0);
        if (p == 0)      clear_region(t, t->cur_col, t->cur_row, t->cols - 1, t->cur_row);
        else if (p == 1) clear_region(t, 0, t->cur_row, t->cur_col, t->cur_row);
        else             clear_region(t, 0, t->cur_row, t->cols - 1, t->cur_row);
        break;
    }
    case 'L': scroll_down(t, t->cur_row, t->scroll_bot, n); break;   // insert lines
    case 'M': scroll_up(t, t->cur_row, t->scroll_bot, n); break;     // delete lines
    case 'P': {                                                      // delete chars
        term_cell_t *row = cell(t, 0, t->cur_row);
        int rem = t->cols - t->cur_col - n;
        if (rem > 0)
            memmove(&row[t->cur_col], &row[t->cur_col + n], rem * sizeof(term_cell_t));
        for (int c = t->cols - n; c < t->cols; c++)
            if (c >= 0) blank_cell(t, &row[c]);
        t->dirty[t->cur_row] = true;
        break;
    }
    case '@': {                                                      // insert chars
        term_cell_t *row = cell(t, 0, t->cur_row);
        int rem = t->cols - t->cur_col - n;
        if (rem > 0)
            memmove(&row[t->cur_col + n], &row[t->cur_col], rem * sizeof(term_cell_t));
        for (int c = t->cur_col; c < t->cur_col + n && c < t->cols; c++)
            blank_cell(t, &row[c]);
        t->dirty[t->cur_row] = true;
        break;
    }
    case 'X': {                                                      // erase chars
        for (int c = t->cur_col; c < t->cur_col + n && c < t->cols; c++)
            blank_cell(t, cell(t, c, t->cur_row));
        t->dirty[t->cur_row] = true;
        break;
    }
    case 'S': scroll_up(t, t->scroll_top, t->scroll_bot, n); break;
    case 'T': scroll_down(t, t->scroll_top, t->scroll_bot, n); break;
    case 'r':
        t->scroll_top = param(t, 0, 1) - 1;
        t->scroll_bot = param(t, 1, TERM_ROWS) - 1;
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bot >= TERM_ROWS) t->scroll_bot = TERM_ROWS - 1;
        if (t->scroll_top >= t->scroll_bot) { t->scroll_top = 0; t->scroll_bot = TERM_ROWS - 1; }
        t->cur_col = 0; t->cur_row = 0;
        break;
    case 'm': do_sgr(t); break;
    case 'h': do_mode(t, true); break;
    case 'l': do_mode(t, false); break;
    case 's': t->saved_col = t->cur_col; t->saved_row = t->cur_row; break;
    case 'u': t->cur_col = t->saved_col; t->cur_row = t->saved_row; clamp_cursor(t); break;
    case 'n':
        if (param(t, 0, 0) == 6) {                       // DSR: report cursor
            char buf[32];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", t->cur_row + 1, t->cur_col + 1);
            respond(t, buf);
        }
        break;
    case 'c': respond(t, "\x1b[?6c"); break;             // DA: "VT102"
    case 't': break;                                     // window ops, ignore
    default:
        ESP_LOGD(TAG, "CSI %c ignored", final);
    }
}

// ---------------------------------------------------------------- parser

static void parse_byte(term_t *t, uint8_t b)
{
    // C0 controls act in any state except OSC string body
    if (t->state != ST_OSC && b < 0x20) {
        switch (b) {
        case 0x1b: t->state = ST_ESC; return;
        case '\n': case 0x0b: case 0x0c: linefeed(t); return;
        case '\r': t->cur_col = 0; return;
        case '\b': if (t->cur_col > 0) t->cur_col--; return;
        case '\t': t->cur_col = (t->cur_col / 8 + 1) * 8; clamp_cursor(t); return;
        case 0x07: return;        // BEL: TODO hook up Tab5 speaker beep
        default: return;
        }
    }

    switch (t->state) {
    case ST_GROUND: {
        // UTF-8 decode
        if (t->utf_left > 0) {
            if ((b & 0xc0) == 0x80) {
                t->utf_cp = (t->utf_cp << 6) | (b & 0x3f);
                if (--t->utf_left == 0) put_char(t, t->utf_cp);
            } else {
                t->utf_left = 0;
                put_char(t, 0xfffd);
            }
            return;
        }
        if (b < 0x80)            put_char(t, b);
        else if ((b & 0xe0) == 0xc0) { t->utf_cp = b & 0x1f; t->utf_left = 1; }
        else if ((b & 0xf0) == 0xe0) { t->utf_cp = b & 0x0f; t->utf_left = 2; }
        else if ((b & 0xf8) == 0xf0) { t->utf_cp = b & 0x07; t->utf_left = 3; }
        else put_char(t, 0xfffd);
        return;
    }
    case ST_ESC:
        switch (b) {
        case '[':
            t->state = ST_CSI;
            t->nparams = 0;
            memset(t->params, -1, sizeof(t->params));
            t->csi_private = false;
            return;
        case ']': t->state = ST_OSC; t->osc_len = 0; return;
        case '(': case ')': t->state = ST_CHARSET; return;
        case '7': t->saved_col = t->cur_col; t->saved_row = t->cur_row; break;
        case '8': t->cur_col = t->saved_col; t->cur_row = t->saved_row; break;
        case 'D': linefeed(t); break;
        case 'M':                                       // reverse index
            if (t->cur_row == t->scroll_top) scroll_down(t, t->scroll_top, t->scroll_bot, 1);
            else t->cur_row--;
            break;
        case 'c':                                       // full reset
            t->fg = DEFAULT_FG; t->bg = DEFAULT_BG; t->attrs = 0;
            t->scroll_top = 0; t->scroll_bot = TERM_ROWS - 1;
            t->cur_col = t->cur_row = 0;
            clear_region(t, 0, 0, t->cols - 1, TERM_ROWS - 1);
            break;
        case '=': case '>': break;                      // keypad modes
        default: ESP_LOGD(TAG, "ESC %c ignored", b);
        }
        t->state = ST_GROUND;
        return;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 0) *p = 0;
            *p = *p * 10 + (b - '0');
        } else if (b == ';') {
            if (t->nparams < MAX_PARAMS) t->nparams++;
        } else if (b == '?') {
            t->csi_private = true;
        } else if (b == '>' || b == '<' || b == '=' || b == ' ' || b == '!'
                   || b == '"' || b == '\'') {
            // intermediate/private markers we don't act on
        } else if (b >= 0x40 && b <= 0x7e) {
            if (t->nparams == 0) t->nparams = 1;
            do_csi(t, (char)b);
            t->state = ST_GROUND;
        } else {
            t->state = ST_GROUND;
        }
        return;
    case ST_OSC:
        // consume until BEL or ESC \ ; titles etc. are ignored
        if (b == 0x07) { t->state = ST_GROUND; return; }
        if (b == 0x1b) { t->state = ST_ESC; return; }   // ST terminator: ESC eats '\'
        if (t->osc_len < (int)sizeof(t->osc_buf) - 1) t->osc_buf[t->osc_len++] = b;
        return;
    case ST_CHARSET:
        t->state = ST_GROUND;   // ignore charset designation byte
        return;
    }
}

// ---------------------------------------------------------------- API

term_t *term_create(void)
{
    term_t *t = heap_caps_calloc(1, sizeof(term_t), MALLOC_CAP_DEFAULT);
    size_t gsz = TERM_COLS_MAX * TERM_ROWS * sizeof(term_cell_t);
    t->main_grid = heap_caps_malloc(gsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    t->alt_grid  = heap_caps_malloc(gsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    t->grid = t->main_grid;
    t->cols = TERM_COLS_PANEL;
    t->fg = DEFAULT_FG;
    t->bg = DEFAULT_BG;
    t->cursor_visible = true;
    t->scroll_top = 0;
    t->scroll_bot = TERM_ROWS - 1;
    t->mutex = xSemaphoreCreateMutex();
    clear_region(t, 0, 0, t->cols - 1, TERM_ROWS - 1);
    // alt grid cleared on first entry
    ESP_LOGI(TAG, "terminal %dx%d (max %d), %u bytes/grid", t->cols, TERM_ROWS, TERM_COLS_MAX, (unsigned)gsz);
    return t;
}

void term_set_response_cb(term_t *t, term_response_cb_t cb, void *ctx)
{
    t->resp_cb = cb;
    t->resp_ctx = ctx;
}

void term_feed(term_t *t, const uint8_t *data, size_t len)
{
    xSemaphoreTake(t->mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) parse_byte(t, data[i]);
    g_dbg_term_feed += len;
    xSemaphoreGive(t->mutex);
}

int term_cols(term_t *t) { return t->cols; }

void term_redraw(term_t *t)
{
    xSemaphoreTake(t->mutex, portMAX_DELAY);
    all_dirty(t);
    xSemaphoreGive(t->mutex);
}

void term_set_cols(term_t *t, int cols)
{
    if (cols < 2) cols = 2;
    if (cols > TERM_COLS_MAX) cols = TERM_COLS_MAX;
    xSemaphoreTake(t->mutex, portMAX_DELAY);
    if (cols != t->cols) {
        t->cols = cols;
        t->scroll_top = 0;
        t->scroll_bot = TERM_ROWS - 1;
        clear_region(t, 0, 0, t->cols - 1, TERM_ROWS - 1);
        t->cur_col = 0;
        t->cur_row = 0;
        all_dirty(t);
    }
    xSemaphoreGive(t->mutex);
}

void term_lock(term_t *t)   { xSemaphoreTake(t->mutex, portMAX_DELAY); }
void term_unlock(term_t *t) { xSemaphoreGive(t->mutex); }

const term_cell_t *term_row(term_t *t, int row) { return &t->grid[row * TERM_COLS_MAX]; }
bool term_row_dirty(term_t *t, int row)         { return t->dirty[row]; }
void term_clear_dirty(term_t *t)                { memset(t->dirty, 0, sizeof(t->dirty)); }
bool term_alt_screen(term_t *t)                 { return t->alt_active; }

void term_cursor(term_t *t, int *col, int *row, bool *visible)
{
    *col = t->cur_col >= t->cols ? t->cols - 1 : t->cur_col;
    *row = t->cur_row;
    *visible = t->cursor_visible;
}

int term_encode_key(term_t *t, term_key_t key, char *buf, size_t buflen)
{
    const char *seq = NULL;
    char c = 0;
    switch (key) {
    case TERM_KEY_UP:    c = 'A'; break;
    case TERM_KEY_DOWN:  c = 'B'; break;
    case TERM_KEY_RIGHT: c = 'C'; break;
    case TERM_KEY_LEFT:  c = 'D'; break;
    case TERM_KEY_HOME:  c = 'H'; break;
    case TERM_KEY_END:   c = 'F'; break;
    case TERM_KEY_PGUP:   seq = "\x1b[5~"; break;
    case TERM_KEY_PGDN:   seq = "\x1b[6~"; break;
    case TERM_KEY_DELETE: seq = "\x1b[3~"; break;
    case TERM_KEY_INSERT: seq = "\x1b[2~"; break;
    }
    int n;
    if (seq) {
        n = snprintf(buf, buflen, "%s", seq);
    } else {
        n = snprintf(buf, buflen, "\x1b%c%c", t->app_cursor_keys ? 'O' : '[', c);
    }
    return n;
}
