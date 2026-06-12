// Draws the term_t cell grid onto a full-screen LVGL canvas.
// Strategy: per dirty row, coalesce runs of cells sharing fg/bg/attrs and
// draw one background rect + one label per run. unscii-16 (8x16 mono) at
// 1280x720 gives exactly 160x45 cells.

#include "term_render.h"
#include "assets.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "term_render";

#define CELL_W TERM_CELL_W
#define CELL_H TERM_CELL_H
#define SCREEN_W (TERM_COLS_MAX * CELL_W)   // canvas always full width
#define SCREEN_H (TERM_ROWS * CELL_H)
#define REFRESH_MS 33

#if CONFIG_TAB5_FONT_SMALL
#define TERM_FONT (&lv_font_unscii_16)
#elif CONFIG_TAB5_FONT_LARGE
LV_FONT_DECLARE(jbmono_30);
#define TERM_FONT (&jbmono_30)
#else
LV_FONT_DECLARE(jbmono_20);
#define TERM_FONT (&jbmono_20)
// CJK companion font (GB2312 + CJK punctuation), 24px to match the 12x24 cell.
LV_FONT_DECLARE(cjk24);
#define TERM_FONT_CJK (&cjk24)
#endif

static term_t *s_term;
static lv_obj_t *s_canvas;
static const lv_font_t *s_font_ascii;       // fitted copies (line_height == CELL_H)
static const lv_font_t *s_font_cjk_flash;
static const lv_font_t *s_font_cjk_sd;
static const lv_font_t *s_font_nerd_sd;
static uint8_t *s_canvas_buf;
static int s_last_cur_col = -1, s_last_cur_row = -1;
volatile uint32_t g_dbg_render_ticks;

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = 0xc0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3f);
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = 0xe0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3f);
        out[2] = 0x80 | (cp & 0x3f);
        return 3;
    }
    out[0] = 0xf0 | (cp >> 18);
    out[1] = 0x80 | ((cp >> 12) & 0x3f);
    out[2] = 0x80 | ((cp >> 6) & 0x3f);
    out[3] = 0x80 | (cp & 0x3f);
    return 4;
}

static void fill_bg(lv_layer_t *layer, uint32_t bg, int c0, int c1, int row)
{
    lv_area_t area = {
        .x1 = c0 * CELL_W, .y1 = row * CELL_H,
        .x2 = c1 * CELL_W - 1, .y2 = (row + 1) * CELL_H - 1,
    };
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(bg);
    rd.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rd, &area);
}

static void draw_text(lv_layer_t *layer, const char *text, uint32_t fg,
                      const lv_font_t *font, int c0, int c1, int row)
{
    lv_area_t area = {
        .x1 = c0 * CELL_W, .y1 = row * CELL_H,
        .x2 = c1 * CELL_W - 1, .y2 = (row + 1) * CELL_H - 1,
    };
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.text = text;
    ld.text_local = 1;   // deferred draw: LVGL must copy the stack buffer
    ld.color = lv_color_hex(fg);
    ld.font = font;
    lv_draw_label(layer, &ld, &area);
}

// Runtime metric fixup: terminal cells are exactly CELL_H tall, but fonts
// ship with their own (often larger) line height — JetBrains Mono reports 32
// for size 20 — which makes LVGL place the baseline below the cell and clip
// descenders. Make a RAM copy with line_height == CELL_H and the original
// baseline position re-centered.
static const lv_font_t *fit_font(const lv_font_t *src)
{
    if (!src) return NULL;
    lv_font_t *f = malloc(sizeof(lv_font_t));
    *f = *src;
    int pos = (int)src->line_height - (int)src->base_line;   // baseline from top
    int newpos = pos + (CELL_H - (int)src->line_height) / 2;
    if (newpos < 1) newpos = 1;
    f->line_height = CELL_H;
    f->base_line = CELL_H - newpos;
    return f;
}

static const lv_font_t *cjk_sd(void)
{
    if (!s_font_cjk_sd && assets_font_cjk()) s_font_cjk_sd = fit_font(assets_font_cjk());
    return s_font_cjk_sd;
}

static const lv_font_t *nerd_sd(void)
{
    if (!s_font_nerd_sd && assets_font_nerd()) s_font_nerd_sd = fit_font(assets_font_nerd());
    return s_font_nerd_sd;
}

// Powerline separators drawn as geometry: pixel-perfect, seamless segment
// joins regardless of font metrics. fg forms the shape, bg fills the rest.
static bool draw_powerline(lv_layer_t *layer, uint32_t cp, uint32_t fg, uint32_t bg,
                           int c, int row)
{
    if (cp < 0xE0B0 || cp > 0xE0B7 || (cp & 1)) return false;   // odd = thin variants -> font

    int x = c * CELL_W, y = row * CELL_H;
    fill_bg(layer, bg, c, c + 1, row);

    if (cp == 0xE0B0 || cp == 0xE0B2) {              // solid triangles
        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.color = lv_color_hex(fg);
        td.opa = LV_OPA_COVER;
        if (cp == 0xE0B0) {                          // points right
            td.p[0].x = x;               td.p[0].y = y;
            td.p[1].x = x;               td.p[1].y = y + CELL_H;
            td.p[2].x = x + CELL_W;      td.p[2].y = y + CELL_H / 2;
        } else {                                     // points left
            td.p[0].x = x + CELL_W;      td.p[0].y = y;
            td.p[1].x = x + CELL_W;      td.p[1].y = y + CELL_H;
            td.p[2].x = x;               td.p[2].y = y + CELL_H / 2;
        }
        lv_draw_triangle(layer, &td);
        return true;
    }

    // E0B4 / E0B6: half-pill caps. Draw a full circle wider than the cell and
    // let a temporarily narrowed layer clip keep just our half.
    lv_area_t saved_clip = layer->_clip_area;
    lv_area_t cell_clip = { .x1 = x, .y1 = y, .x2 = x + CELL_W - 1, .y2 = y + CELL_H - 1 };
    layer->_clip_area = cell_clip;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(fg);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t area;
    if (cp == 0xE0B4) {                              // right rounded cap
        area = (lv_area_t){ .x1 = x - CELL_W, .y1 = y, .x2 = x + CELL_W - 1, .y2 = y + CELL_H - 1 };
    } else {                                         // E0B6: left rounded cap
        area = (lv_area_t){ .x1 = x, .y1 = y, .x2 = x + 2 * CELL_W - 1, .y2 = y + CELL_H - 1 };
    }
    lv_draw_rect(layer, &rd, &area);
    layer->_clip_area = saved_clip;
    return true;
}

// True if the font itself (no fallback/placeholder) has a glyph for cp.
// This is the hang guard: LVGL canvas draw tasks for glyphs that no font
// provides are never consumed and deadlock lv_canvas_finish_layer.
static bool font_has(const lv_font_t *f, uint32_t cp)
{
    if (!f) return false;
    lv_font_glyph_dsc_t g;
    return f->get_glyph_dsc((lv_font_t *)f, &g, cp, 0);
}

static uint32_t bold_color(uint32_t fg)
{
    uint32_t r = (fg >> 16) & 0xff, g = (fg >> 8) & 0xff, b = fg & 0xff;
    r += (255 - r) * 2 / 5;
    g += (255 - g) * 2 / 5;
    b += (255 - b) * 2 / 5;
    return (r << 16) | (g << 8) | b;
}

static void draw_underline(lv_layer_t *layer, uint32_t fg, int c0, int c1, int row)
{
    lv_area_t area = {
        .x1 = c0 * CELL_W, .y1 = (row + 1) * CELL_H - 2,
        .x2 = c1 * CELL_W - 1, .y2 = (row + 1) * CELL_H - 1,
    };
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(fg);
    rd.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rd, &area);
}

// Single cell/glyph draw with font fallback chain. Never submits a glyph
// that no font covers (draws '?' instead). Returns true if the glyph is a
// nerd-font icon that should be redrawn after the row with overflow allowed
// (the icons are em-wide but terminals allocate one cell).
static bool draw_one(lv_layer_t *layer, const term_cell_t *cell, int c, int row)
{
    uint32_t cp = cell->ch;
    int w = term_cp_width(cp);
    uint32_t fg = (cell->attrs & TERM_ATTR_BOLD) ? bold_color(cell->fg) : cell->fg;

    // powerline separators: geometry, raw fg (bold lightening would break the
    // color match with the adjacent segment background)
    if (draw_powerline(layer, cp, cell->fg, cell->bg, c, row)) return false;

    fill_bg(layer, cell->bg, c, c + w, row);

#ifdef TERM_FONT_CJK
    // color emoji atlas (SD card). Images are 24px wide; when the terminal
    // allocated just one cell (BMP symbols like U+26A0), defer to the
    // overflow pass so the right half isn't painted over by the next cell.
    const lv_image_dsc_t *img = assets_emoji(cp);
    if (img) {
        if (w == 1) return true;
        lv_area_t area = {
            .x1 = c * CELL_W, .y1 = row * CELL_H,
            .x2 = c * CELL_W + img->header.w - 1,
            .y2 = row * CELL_H + img->header.h - 1,
        };
        lv_draw_image_dsc_t id;
        lv_draw_image_dsc_init(&id);
        id.src = img;
        lv_draw_image(layer, &id, &area);
        if (cell->attrs & TERM_ATTR_UNDERLINE) draw_underline(layer, fg, c, c + w, row);
        return false;
    }
#endif

    // common TUI symbols missing from all fonts -> geometric equivalents
    static const uint32_t alias[][2] = {
        { 0x23F4, 0x25C0 }, { 0x23F5, 0x25B6 },
        { 0x23F6, 0x25B2 }, { 0x23F7, 0x25BC },
        { 0x2022, 0x00B7 }, { 0x25CF, 0x2588 },
    };

    const lv_font_t *chain[] = {
        s_font_ascii,
#ifdef TERM_FONT_CJK
        cjk_sd(),
        s_font_cjk_flash,
#endif
        nerd_sd(),
    };
    const lv_font_t *font = NULL;
    for (int pass = 0; pass < 2 && !font; pass++) {
        for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
            if (font_has(chain[i], cp)) { font = chain[i]; break; }
        }
        if (!font && pass == 0) {
            uint32_t orig = cp;
            for (size_t a = 0; a < sizeof(alias) / sizeof(alias[0]); a++) {
                if (alias[a][0] == orig) { cp = alias[a][1]; break; }
            }
            if (cp == orig) break;
        }
    }

    char gbuf[5];
    if (font) {
        gbuf[utf8_encode(cp, gbuf)] = 0;
    } else {
        gbuf[0] = '?';
        gbuf[1] = 0;
        font = s_font_ascii;
    }

    // Nerd icons are em-square but occupy one terminal cell: background is
    // done (above); the glyph itself is drawn in a second pass over the
    // finished row so it may overflow into the neighbour cell like desktop
    // terminals allow.
    if (font == nerd_sd() && w == 1) return true;

    draw_text(layer, gbuf, fg, font, c, c + w, row);
    if (cell->attrs & TERM_ATTR_UNDERLINE) draw_underline(layer, fg, c, c + w, row);
    return false;
}

#define MAX_ROW_ICONS 32

static void draw_row(lv_layer_t *layer, int row)
{
    const term_cell_t *cells = term_row(s_term, row);
    const int ncols = term_cols(s_term);
    char runbuf[TERM_COLS_MAX * 4 + 1];
    int icon_cols[MAX_ROW_ICONS];
    int n_icons = 0;

    int c = 0;
    while (c < ncols) {
        uint32_t cp = cells[c].ch;

        if (cp == 0) {                  // orphaned continuation cell
            fill_bg(layer, cells[c].bg, c, c + 1, row);
            c++;
            continue;
        }

        // anything beyond printable ASCII: per-cell draw with fallback chain
        if (cp < 0x20 || cp > 0x7e) {
            if (draw_one(layer, &cells[c], c, row) && n_icons < MAX_ROW_ICONS) {
                icon_cols[n_icons++] = c;
            }
            c += term_cp_width(cp);
            continue;
        }

        // printable-ASCII run: batch cells sharing colors+attrs into one draw
        int start = c;
        uint32_t fg = cells[c].fg, bg = cells[c].bg;
        uint8_t attrs = cells[c].attrs;
        int len = 0;
        while (c < ncols && cells[c].ch >= 0x20 && cells[c].ch <= 0x7e
               && cells[c].fg == fg && cells[c].bg == bg
               && cells[c].attrs == attrs) {
            runbuf[len++] = (char)cells[c].ch;
            c++;
        }
        runbuf[len] = 0;

        uint32_t efg = (attrs & TERM_ATTR_BOLD) ? bold_color(fg) : fg;
        fill_bg(layer, bg, start, c, row);
        draw_text(layer, runbuf, efg, s_font_ascii, start, c, row);
        if (attrs & TERM_ATTR_UNDERLINE) draw_underline(layer, efg, start, c, row);
    }

    // second pass: em-wide nerd icons / one-cell emoji, drawn over the
    // finished row with one extra cell of horizontal room
    for (int i = 0; i < n_icons; i++) {
        int ic = icon_cols[i];
        const term_cell_t *cell = &cells[ic];
        const lv_image_dsc_t *img = assets_emoji(cell->ch);
        if (img) {
            lv_area_t area = {
                .x1 = ic * CELL_W, .y1 = row * CELL_H,
                .x2 = ic * CELL_W + img->header.w - 1,
                .y2 = row * CELL_H + img->header.h - 1,
            };
            lv_draw_image_dsc_t id;
            lv_draw_image_dsc_init(&id);
            id.src = img;
            lv_draw_image(layer, &id, &area);
            continue;
        }
        char gbuf[5];
        gbuf[utf8_encode(cell->ch, gbuf)] = 0;
        uint32_t fg = (cell->attrs & TERM_ATTR_BOLD) ? bold_color(cell->fg) : cell->fg;
        draw_text(layer, gbuf, fg, nerd_sd(), ic, ic + 2, row);
    }
}

static void draw_cursor(lv_layer_t *layer, int col, int row)
{
    lv_area_t area = {
        .x1 = col * CELL_W, .y1 = row * CELL_H,
        .x2 = (col + 1) * CELL_W - 1, .y2 = (row + 1) * CELL_H - 1,
    };
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(0xd0d0d0);
    rd.bg_opa = LV_OPA_60;
    lv_draw_rect(layer, &rd, &area);
}

static void refresh_cb(lv_timer_t *timer)
{
    g_dbg_render_ticks++;
    int64_t t0 = esp_timer_get_time();
    int cur_col, cur_row;
    bool cur_vis;

    term_lock(s_term);

    bool any = false;
    lv_layer_t layer;
    term_cursor(s_term, &cur_col, &cur_row, &cur_vis);
    bool cursor_moved = (cur_col != s_last_cur_col || cur_row != s_last_cur_row);

    for (int r = 0; r < TERM_ROWS; r++) {
        bool need = term_row_dirty(s_term, r)
                    || (cursor_moved && (r == cur_row || r == s_last_cur_row));
        if (!need) continue;
        if (!any) { lv_canvas_init_layer(s_canvas, &layer); any = true; }
        draw_row(&layer, r);
        if (cur_vis && r == cur_row) draw_cursor(&layer, cur_col, cur_row);
    }
    if (any) {
        lv_canvas_finish_layer(s_canvas, &layer);
        term_clear_dirty(s_term);
        s_last_cur_col = cur_col;
        s_last_cur_row = cur_row;
        lv_obj_invalidate(s_canvas);
    }

    term_unlock(s_term);

    int64_t dt = esp_timer_get_time() - t0;
    if (dt > 200000) {
        ESP_LOGW(TAG, "slow render pass: %lld ms", dt / 1000);
    }
}

const uint8_t *term_render_framebuffer(int *w, int *h, int *stride_bytes)
{
    if (!s_canvas) return NULL;
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(s_canvas);
    *w = SCREEN_W;
    *h = SCREEN_H;
    *stride_bytes = db ? (int)db->header.stride : SCREEN_W * 2;
    return s_canvas_buf;
}

void term_render_start(term_t *t)
{
    s_term = t;
    s_font_ascii = fit_font(TERM_FONT);
#ifdef TERM_FONT_CJK
    s_font_cjk_flash = fit_font(TERM_FONT_CJK);
#endif

    size_t buf_sz = LV_CANVAS_BUF_SIZE(SCREEN_W, SCREEN_H, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_canvas_buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "canvas %dx%d, %u KB", SCREEN_W, SCREEN_H, (unsigned)(buf_sz / 1024));

    bsp_display_lock(0);
    // Terminal background color also fills the sliver between the canvas
    // edge and the panel, so no stray screen background shows through.
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101010), 0);
    s_canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, SCREEN_W, SCREEN_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x101010), LV_OPA_COVER);
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    bsp_display_unlock();
}
