// Boot animation: terminal-style typewriter logo. Mirrors the design in
// tools/bootanim_preview.py — blinking cursor, "tab5" typed out, cyan
// powerline sweep, "SSH TERMINAL" subtitle fade, then the whole layer fades
// away. Runs on the LVGL top layer while the rest of boot continues
// underneath; everything self-deletes at the end.
//
// Timeline (ms):  0-400 cursor only | 400-1200 typing | 1200-1600 sweep
//                 1600-2200 subtitle fade | 2300-3050 case/italic shuffle |
//                 3050-3900 hold on "tab5" | 3900-4300 fade out
#include "boot_anim.h"

#include "bsp/esp-bsp.h"
#include "sfx.h"
#include "lvgl.h"

LV_FONT_DECLARE(bootlogo150);
LV_FONT_DECLARE(bootlogo150i);   // italic variant for the shuffle beat
LV_FONT_DECLARE(bootsub34);

#define COL_BG    0x0d0d0d
#define COL_FG    0xdcdcdc
#define COL_CYAN  0x3ec7c0
#define COL_RED   0xe05252   // sweep line
#define COL_GREY  0x787878

#define LOGO_Y    180          // logo top on the 720px screen
#define SWEEP_H   6
#define CURSOR_GAP 12          // space between last glyph and cursor

// Cursor box derived from font metrics at init (slimmer than a full cell,
// bottom sits on the text baseline).
static int32_t s_cur_w, s_cur_h, s_cur_y;

static lv_obj_t *s_root, *s_logo, *s_cursor, *s_sweep, *s_sub;
static volatile bool s_ready;   // boot_anim_finish() releases the splash
static int s_fade_start;        // tick the fade-out began
static lv_timer_t *s_timer;
static int s_tick;             // 50ms ticks

static const char *s_text = "tab5";

// lv_text_get_width is private in LVGL 9.2; glyph advances are all we need.
static int32_t text_w(const char *s, int n, const lv_font_t *f, int32_t lsp)
{
    int32_t w = 0;
    for (int i = 0; i < n; i++) {
        w += lv_font_get_glyph_width(f, (uint32_t)s[i],
                                     (uint32_t)(i + 1 < n ? s[i + 1] : 0));
        if (i + 1 < n) w += lsp;
    }
    return w;
}

static int32_t logo_text_w(int nchars)
{
    return text_w(s_text, nchars, &bootlogo150, 0);
}

static void tick_cb(lv_timer_t *tm)
{
    s_tick++;
    int ms = s_tick * 50;

    // typing: one char per 200ms starting at 400ms
    int want = 0;
    if (ms >= 400) {
        want = (ms - 400) / 200 + 1;
        if (want > 4) want = 4;
    }
    static int have = -1;
    if (want != have) {
        have = want;
        char buf[8] = {0};
        memcpy(buf, s_text, want);
        lv_label_set_text(s_logo, buf);
        lv_obj_set_x(s_cursor,
                     lv_obj_get_x(s_logo) + logo_text_w(want) + CURSOR_GAP);
        if (want > 0) sfx_play(SFX_CLICK);
    }

    // cursor blink, 600ms period; solid white once typing is done
    bool on = (s_tick % 12) < 7;
    lv_obj_set_style_bg_color(s_cursor,
        lv_color_hex(want < 4 ? COL_CYAN : COL_FG), 0);
    if (on) lv_obj_remove_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

    // powerline sweep 1200-1600ms — spans exactly the text width
    if (ms >= 1200) {
        int32_t full = logo_text_w(4);
        int32_t w = (ms >= 1600) ? full : full * (ms - 1200) / 400;
        if (w < 2) w = 2;
        lv_obj_remove_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_sweep, w);
    }

    // subtitle fade 1600-2200ms
    if (ms >= 1600) {
        int32_t opa = (ms >= 2200) ? 255 : 255 * (ms - 1600) / 600;
        lv_obj_remove_flag(s_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_sub, opa, 0);
    }

    // 2300-3050ms: playful shuffle — the glyphs flip through case/italic
    // variants every 150ms and land back on plain lowercase.
    if (ms >= 2300 && ms < 3050) {
        static const struct { const char *txt; bool ital; } beats[] = {
            { "tAb5", false }, { "TAB5", true  }, { "taB5", false },
            { "TAb5", false }, { "tab5", true },   // settles on italic
        };
        int b = (ms - 2300) / 150;
        if (b > 4) b = 4;
        static int last_b = -1;
        if (b != last_b) {
            last_b = b;
            lv_obj_set_style_text_font(s_logo,
                beats[b].ital ? &bootlogo150i : &bootlogo150, 0);
            lv_label_set_text(s_logo, beats[b].txt);
            sfx_play(b == 4 ? SFX_DING : SFX_TICK);
        }
    }

    // Still playing the intro (typing/sweep/shuffle): nothing else to do.
    if (ms < 3900) return;
    // Logo settled: HOLD as a splash until boot_anim_finish() releases it (the
    // rest of boot — assets/Wi-Fi init — can take >10s and we must never reveal
    // the terminal underneath). Safety auto-fade after 15s if finish() never
    // fires.
    if (!s_ready && ms < 15000) return;

    if (s_fade_start == 0) s_fade_start = s_tick;        // first fade tick
    int32_t fade_ms = (s_tick - s_fade_start) * 50;
    int32_t opa = 255 - 255 * fade_ms / 400;
    if (opa <= 0) {
        lv_timer_delete(s_timer);
        lv_obj_delete(s_root);
        s_root = NULL;
        s_timer = NULL;
        return;
    }
    lv_obj_set_style_opa(s_root, opa, 0);
}

// Called when boot is done and the home panel is up — releases the splash.
void boot_anim_finish(void)
{
    s_ready = true;
}

void boot_anim_start(void)
{
    bsp_display_lock(0);

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Cursor: ~45% of a glyph advance, capital-ish height, bottom on the
    // baseline. The layout centers on the text alone; the cursor hangs off
    // the right like a real prompt.
    int32_t adv = lv_font_get_glyph_width(&bootlogo150, 'a', 0);
    s_cur_w = adv * 18 / 100;           // slim beam-style cursor
    s_cur_h = bootlogo150.line_height - bootlogo150.base_line;  // ascent box
    s_cur_h = s_cur_h * 78 / 100;                               // visual trim
    int32_t baseline = LOGO_Y + bootlogo150.line_height - bootlogo150.base_line;
    s_cur_y = baseline - s_cur_h;

    int32_t full_w = logo_text_w(4);
    int32_t x0 = (1280 - full_w) / 2;

    s_logo = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_logo, &bootlogo150, 0);
    lv_obj_set_style_text_color(s_logo, lv_color_hex(COL_FG), 0);
    lv_label_set_text(s_logo, "");
    lv_obj_set_pos(s_logo, x0, LOGO_Y);

    s_cursor = lv_obj_create(s_root);
    lv_obj_set_size(s_cursor, s_cur_w, s_cur_h);
    lv_obj_set_pos(s_cursor, x0 + CURSOR_GAP, s_cur_y);
    lv_obj_set_style_radius(s_cursor, 0, 0);
    lv_obj_set_style_border_width(s_cursor, 0, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(COL_CYAN), 0);

    s_sweep = lv_obj_create(s_root);
    lv_obj_set_size(s_sweep, 2, SWEEP_H);
    lv_obj_set_pos(s_sweep, x0, baseline + 40);
    lv_obj_set_style_radius(s_sweep, 0, 0);
    lv_obj_set_style_border_width(s_sweep, 0, 0);
    lv_obj_set_style_bg_color(s_sweep, lv_color_hex(COL_RED), 0);
    lv_obj_add_flag(s_sweep, LV_OBJ_FLAG_HIDDEN);

    s_sub = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_sub, &bootsub34, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(COL_GREY), 0);
    lv_obj_set_style_text_letter_space(s_sub, 14, 0);
    lv_label_set_text(s_sub, "SSH TERMINAL");
    lv_obj_set_style_opa(s_sub, 0, 0);
    int32_t sub_w = text_w("SSH TERMINAL", 12, &bootsub34, 14);
    lv_obj_set_pos(s_sub, (1280 - sub_w) / 2, baseline + 90);
    lv_obj_add_flag(s_sub, LV_OBJ_FLAG_HIDDEN);

    s_tick = 0;
    s_timer = lv_timer_create(tick_cb, 50, NULL);

    bsp_display_unlock();
}
