// Pinyin IME candidate bar: 1280x52 strip pinned to the bottom of the
// screen, hidden unless a composition is active. Layout:
//   [pinyin]  [1 你好] [2 尼] ... (up to 7)  [<] [>]
//
// LOCK ORDERING (see ime_filter.cpp): the touch callbacks below run inside
// the LVGL task and call straight into ime_filter_pick()/_page() without
// holding anything; ime_filter only calls ime_bar_show()/_hide() AFTER
// releasing its engine mutex. ime_bar takes only bsp_display_lock (the
// esp_lvgl_port lock is recursive, so calls from the LVGL task are fine).

#include "ime_bar.h"

#include <stdio.h>

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "ime_filter.h"
#include "assets.h"

#define BAR_W 1280
#define BAR_H 52

LV_FONT_DECLARE(cjk24);     // built-in fallback when no SD cjkfull24.bin

static lv_obj_t *s_bar;
static lv_obj_t *s_comp_label;              // spelling string ("nihao")
static lv_obj_t *s_cand_btn[IME_BAR_CANDS];
static lv_obj_t *s_cand_num[IME_BAR_CANDS]; // "1".."7" (latin font)
static lv_obj_t *s_cand_txt[IME_BAR_CANDS]; // candidate hanzi (CJK font)
static lv_obj_t *s_prev_btn;
static lv_obj_t *s_next_btn;

static void cand_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ime_filter_pick(idx);
}

static void page_btn_cb(lv_event_t *e)
{
    ime_filter_page((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *mk_btn(lv_obj_t *parent, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, BAR_H - 12);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a36), 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

void ime_bar_create(void)
{
    const lv_font_t *cjk = assets_font_cjk() ? assets_font_cjk() : &cjk24;

    bsp_display_lock(0);

    s_bar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_set_pos(s_bar, 0, 720 - BAR_H);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x16161e), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(0x383848), 0);
    lv_obj_set_style_pad_all(s_bar, 6, 0);
    lv_obj_set_style_pad_column(s_bar, 8, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_comp_label = lv_label_create(s_bar);
    lv_label_set_text(s_comp_label, "");
    lv_obj_set_style_text_font(s_comp_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_comp_label, lv_color_hex(0x80c080), 0);
    lv_obj_set_width(s_comp_label, 180);
    lv_label_set_long_mode(s_comp_label, LV_LABEL_LONG_DOT);

    for (int i = 0; i < IME_BAR_CANDS; i++) {
        lv_obj_t *btn = mk_btn(s_bar, cand_btn_cb, (void *)(intptr_t)i);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 6, 0);

        s_cand_num[i] = lv_label_create(btn);
        lv_label_set_text_fmt(s_cand_num[i], "%d", i + 1);
        lv_obj_set_style_text_font(s_cand_num[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_cand_num[i], lv_color_hex(0x8080a0), 0);

        s_cand_txt[i] = lv_label_create(btn);
        lv_label_set_text(s_cand_txt[i], "");
        lv_obj_set_style_text_font(s_cand_txt[i], cjk, 0);
        lv_obj_set_style_text_color(s_cand_txt[i], lv_color_hex(0xe0e0e0), 0);

        s_cand_btn[i] = btn;
    }

    s_prev_btn = mk_btn(s_bar, page_btn_cb, (void *)(intptr_t)-1);
    lv_obj_t *l = lv_label_create(s_prev_btn);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_center(l);

    s_next_btn = mk_btn(s_bar, page_btn_cb, (void *)(intptr_t)1);
    l = lv_label_create(s_next_btn);
    lv_label_set_text(l, LV_SYMBOL_RIGHT);
    lv_obj_center(l);

    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

void ime_bar_show(const char *pinyin, const char *const cands_utf8[], int n,
                  bool has_prev, bool has_next)
{
    if (!s_bar) return;
    if (n > IME_BAR_CANDS) n = IME_BAR_CANDS;

    bsp_display_lock(0);
    lv_label_set_text(s_comp_label, pinyin ? pinyin : "");
    for (int i = 0; i < IME_BAR_CANDS; i++) {
        if (i < n) {
            lv_label_set_text(s_cand_txt[i], cands_utf8[i]);
            lv_obj_clear_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cand_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (has_prev) lv_obj_clear_flag(s_prev_btn, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(s_prev_btn, LV_OBJ_FLAG_HIDDEN);
    if (has_next) lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bar);
    bsp_display_unlock();
}

void ime_bar_hide(void)
{
    if (!s_bar) return;
    bsp_display_lock(0);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}
