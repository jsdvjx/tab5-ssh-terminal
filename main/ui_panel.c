#include "ui_panel.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "term.h"

#define PANEL_X (1280 - TAB5_PANEL_W)

static lv_obj_t *s_panel;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_url_label;
static lv_obj_t *s_target_list;
static lv_obj_t *s_state_label;

static ui_panel_target_cb_t s_target_cb;

// ---------------------------------------------------------------- creation

void ui_panel_create(void)
{
    bsp_display_lock(0);

    s_panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_panel, TAB5_PANEL_W, 720);
    lv_obj_set_pos(s_panel, PANEL_X, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x1c1c28), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 12, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_panel, 10, 0);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "Tab5 Terminal   (Ctrl+Alt+P)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    s_wifi_label = lv_label_create(s_panel);
    lv_label_set_text(s_wifi_label, "Wi-Fi: --");
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(0xa0c8ff), 0);
    lv_obj_set_width(s_wifi_label, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_label, LV_LABEL_LONG_WRAP);

    s_url_label = lv_label_create(s_panel);
    lv_label_set_text(s_url_label, "");
    lv_obj_set_style_text_color(s_url_label, lv_color_hex(0x80e0a0), 0);
    lv_obj_set_width(s_url_label, LV_PCT(100));
    lv_label_set_long_mode(s_url_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *hdr = lv_label_create(s_panel);
    lv_label_set_text(hdr, "SSH targets (tap to switch)");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x808090), 0);

    s_target_list = lv_list_create(s_panel);
    lv_obj_set_width(s_target_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_target_list, 1);
    lv_obj_set_style_bg_color(s_target_list, lv_color_hex(0x14141e), 0);
    lv_obj_set_style_border_width(s_target_list, 0, 0);

    s_state_label = lv_label_create(s_panel);
    lv_label_set_text(s_state_label, "");
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_width(s_state_label, LV_PCT(100));
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_WRAP);

    bsp_display_unlock();
}

bool ui_panel_toggle(void)
{
    bool visible;
    bsp_display_lock(0);
    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        visible = true;
    } else {
        lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
        visible = false;
    }
    bsp_display_unlock();
    return visible;
}

// ---------------------------------------------------------------- wifi/url

void ui_panel_set_wifi(const char *ssid, const char *ip, int rssi)
{
    if (!s_wifi_label) return;
    char buf[96];
    if (!ssid) {
        snprintf(buf, sizeof(buf), "Wi-Fi: connecting...");
    } else {
        snprintf(buf, sizeof(buf), "Wi-Fi: %s  %ddBm\nIP: %s", ssid, rssi, ip ? ip : "-");
    }
    bsp_display_lock(0);
    lv_label_set_text(s_wifi_label, buf);
    bsp_display_unlock();
}

void ui_panel_set_url(const char *url)
{
    if (!s_url_label) return;
    char buf[96];
    snprintf(buf, sizeof(buf), url ? "Config: %s" : "", url ? url : "");
    bsp_display_lock(0);
    lv_label_set_text(s_url_label, buf);
    bsp_display_unlock();
}

void ui_panel_set_state(const char *text)
{
    if (!s_state_label) return;
    bsp_display_lock(0);
    lv_label_set_text(s_state_label, text ? text : "");
    bsp_display_unlock();
}

// ---------------------------------------------------------------- targets

static void target_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_target_cb) s_target_cb(idx);
}

void ui_panel_set_targets(const settings_t *s, int active, ui_panel_target_cb_t cb)
{
    if (!s_target_list) return;
    s_target_cb = cb;

    bsp_display_lock(0);
    lv_obj_clean(s_target_list);
    for (int i = 0; i < s->n_targets; i++) {
        const ssh_target_t *t = &s->targets[i];
        char line[96];
        snprintf(line, sizeof(line), "%s\n%s@%s:%d",
                 t->name[0] ? t->name : t->host, t->user, t->host, t->port);
        lv_obj_t *btn = lv_list_add_button(s_target_list, LV_SYMBOL_RIGHT, line);
        lv_obj_add_event_cb(btn, target_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(btn, lv_color_hex(i == active ? 0x2a4a2a : 0x14141e), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(i == active ? 0xa0ffa0 : 0xd0d0d0), 0);
    }
    if (s->n_targets == 0) {
        lv_obj_t *btn = lv_list_add_text(s_target_list, "no targets - use web config");
        lv_obj_set_style_text_color(btn, lv_color_hex(0x808080), 0);
    }
    bsp_display_unlock();
}

// ---------------------------------------------------------------- ssid picker

static SemaphoreHandle_t s_pick_sem;
static volatile int s_pick_result;

static void pick_btn_cb(lv_event_t *e)
{
    s_pick_result = (int)(intptr_t)lv_event_get_user_data(e);
    xSemaphoreGive(s_pick_sem);
}

int ui_panel_pick(const char *title_text, const char *names[], int n)
{
    if (!s_pick_sem) s_pick_sem = xSemaphoreCreateBinary();

    bsp_display_lock(0);
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(modal, 500, 600);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x202030), 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    lv_obj_t *list = lv_list_create(modal);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_RIGHT, names[i]);
        lv_obj_add_event_cb(btn, pick_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    bsp_display_unlock();

    xSemaphoreTake(s_pick_sem, portMAX_DELAY);

    bsp_display_lock(0);
    lv_obj_delete(modal);
    bsp_display_unlock();
    return s_pick_result;
}
