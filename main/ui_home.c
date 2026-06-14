// Full-screen home launcher — the primary UI after boot (Ctrl+Alt+P toggles,
// Esc walks back: form -> app -> grid -> terminal).
//
// Visual style: M5Tab5-UserDemo launcher look — warm cream background with
// big, saturated, rounded "toy block" cards and bold dark slab text:
//   background      #FBF3E3  (warm cream)
//   ink / slab text #2B2D42  (near-black navy)
//   card: navy      #344055  (SSH)
//   card: teal      #26C6DA  (Wi-Fi)
//   card: orange    #FF9800  (BLE provisioning)
//   card: yellow    #FFC53D  (display)
//   card: coral     #F4633A  (system)
//   card: green     #4CAF7D  (IME)
//
// Launcher mechanics: the card grid is the desktop. Tapping a card expands
// it to fill the content area; the other cards collapse into a bottom dock
// of icon-only rounded buttons (nerd-font glyphs, each in its card's accent
// color). Tapping a dock icon switches the expanded app; Back / Esc returns
// to the grid.
//
// The SSH app expanded view is a device tile grid: big OS icon (nerd64),
// machine name, status dot. Tap = open-or-switch session and drop to the
// terminal; long-press = edit form; "+" tile = add form. The form has an
// OS picker row (nerd32 icon buttons) and a delete button with inline
// confirm.
//
// The terminal underneath keeps its full width at all times; opening the
// panel never resizes the PTY.

#include "ui_home.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "usb/hid_usage_keyboard.h"

#include "assets.h"
#include "ble_prov.h"
#include "i18n.h"
#include "hid_keyboard.h"
#include "power_mon.h"
#include "ssh_client.h"
#include "ssh_keys.h"
#include "security.h"
#include "status_bar.h"
#include "wifi.h"
#include "nat_tunnel.h"

LV_FONT_DECLARE(cjk24);     // flash fallback when no SD cjkfull24.bin
LV_FONT_DECLARE(nerd32);    // nerd-font icon subset, 32 px (dock, OS picker)
LV_FONT_DECLARE(nerd64);    // same subset, 64 px (device tiles)

#define HOME_W      1280
#define HOME_H      720
#define MARGIN_X    28
#define TOP_H       72
#define GUTTER      20
#define DOCK_H      88
#define CARD_W      ((HOME_W - 2 * MARGIN_X - 2 * GUTTER) / 3)   // 394
#define CARD_H      ((HOME_H - TOP_H - 24 - GUTTER - 12) / 2)    // 296
// Expanded app geometry: fills the content area above the dock.
#define APP_Y       (TOP_H + 12)
#define APP_H       (HOME_H - APP_Y - DOCK_H - 12)
#define APP_W       (HOME_W - 2 * MARGIN_X)

#define COL_BG      0xFBF3E3
#define COL_INK     0x2B2D42
// Muted, coordinated palette (lower saturation than the bright UserDemo
// launcher — the user found the saturated set too flashy). One slate anchor
// + four desaturated companion hues that sit calmly on the cream bg.
#define COL_NAVY    0x3C4858   // SSH (slate anchor)
#define COL_TEAL    0x5B8C92   // 连接 (muted teal)
#define COL_YELLOW  0xC9A35B   // 显示 (muted amber)
#define COL_CORAL   0xB5654B   // 系统 (muted terracotta)
#define COL_GREEN   0x6E9A7C   // 输入法 (muted sage)
#define COL_ORANGE  0xC9803B   // accent (BLE)
#define COL_INK     0x222a33   // dark text on light, buttons on cards
#define COL_INK2    0x1d2a2c   // dark text on teal card

// ------------------------------------------------------------------- apps
// 连接 (APP_CONN) merges the old Wi-Fi and 蓝牙配网 cards into one teal card
// with two side-by-side halves.
enum { APP_SSH, APP_CONN, APP_DISP, APP_SYS, APP_IME, APP_COUNT };

static const uint32_t app_color[APP_COUNT] = {
    COL_NAVY, COL_TEAL, COL_YELLOW, COL_CORAL, COL_GREEN,
};
// Dock glyphs (nerd32): terminal, wifi, lightbulb, cogs, keyboard.
static const char *app_glyph[APP_COUNT] = {
    "\xEF\x84\xA0",   // U+F120 terminal
    "\xEF\x87\xAB",   // U+F1EB wifi (连接)
    "\xEF\x83\xAB",   // U+F0EB lightbulb
    "\xEF\x82\x85",   // U+F085 cogs
    "\xEF\x84\x9C",   // U+F11C keyboard
};
// OS icons (nerd32/nerd64), indexed by target_os_t.
static const char *os_glyph[TARGET_OS_COUNT] = {
    [TARGET_OS_SERVER]    = "\xEF\x88\xB3",   // U+F233 server rack
    [TARGET_OS_APPLE]     = "\xEF\x85\xB9",   // U+F179 apple
    [TARGET_OS_TUX]       = "\xEF\x85\xBC",   // U+F17C tux
    [TARGET_OS_UBUNTU]    = "\xEF\x8C\x9B",   // U+F31B ubuntu
    [TARGET_OS_DEBIAN]    = "\xEF\x8C\x86",   // U+F306 debian
    [TARGET_OS_WINDOWS]   = "\xEF\x85\xBA",   // U+F17A windows
    [TARGET_OS_RASPBERRY] = "\xEF\x8C\x95",   // U+F315 raspberry pi
};

// Color OS-logo atlas (assets_os_icon) with graceful fallback to the mono
// nerd-glyph. Creates an lv_image when the color icon exists, else an
// lv_label with the nerd glyph (using the requested nerd font). Returns the
// created object; the caller positions it. `glyph_color` tints the fallback.
static lv_obj_t *mk_os_icon(lv_obj_t *par, int os, int size,
                            const lv_font_t *nerd_font, uint32_t glyph_color)
{
    if (os < 0 || os >= TARGET_OS_COUNT) os = TARGET_OS_SERVER;
    const lv_image_dsc_t *dsc = assets_os_icon(os, size);
    if (dsc) {
        lv_obj_t *img = lv_image_create(par);
        lv_image_set_src(img, dsc);
        return img;
    }
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_text(l, os_glyph[os]);
    lv_obj_set_style_text_font(l, nerd_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(glyph_color), 0);
    return l;
}

static settings_t *s_cfg;
static lv_obj_t *s_root;
static lv_timer_t *s_timer;

static lv_obj_t *s_clock_label;          // top bar "HH:MM"
static lv_obj_t *s_batt_label;           // top bar "87% ⚡"
// Top-bar Wi-Fi cluster (left of clock/battery): glyph + IP/status text.
static lv_obj_t *s_wifi_top_icon;        // nerd wifi glyph (color-graded)
static lv_obj_t *s_wifi_top_text;        // IP (connected) / 未连接 / 连接中
static lv_timer_t *s_wifi_pulse_timer;   // ~300ms pulse while connecting
static int s_wifi_pulse_phase;
static lv_obj_t *s_card[APP_COUNT];
static lv_obj_t *s_dock;
static lv_obj_t *s_dock_btn[APP_COUNT];

static lv_obj_t *s_manage;              // "管理 ›" chip (grid view only)
static lv_obj_t *s_ssh_list;             // grid-mode target rows
static lv_obj_t *s_ssh_sub;              // "N/M" subtitle
static lv_obj_t *s_tiles;                // expanded-mode device tile grid
// 连接 card — minimal grid face (icon + name per half) + rich expanded detail.
static lv_obj_t *s_conn_face;            // grid face: wifi/bt icon + name (sparse)
static lv_obj_t *s_wifi_face_name;       // "Wi-Fi" or connected SSID
static lv_obj_t *s_conn_detail;          // expanded full-detail container
static lv_obj_t *s_wifi_cur_label;       // current SSID/RSSI/IP (expanded)
static lv_obj_t *s_wifi_list;            // 已存网络 saved-net rows
static lv_obj_t *s_ble_state_label;
static lv_obj_t *s_ble_btn;              // Start/Stop adv (expanded view only)
static lv_obj_t *s_ble_btn_label;
static lv_obj_t *s_ble_master_btn;       // BLE stack master on/off
static lv_obj_t *s_ble_master_label;
// Wi-Fi "添加网络" mini-form overlay (SSID + password).
static lv_obj_t *s_wifiadd;              // overlay (hidden), NULL until built
static lv_obj_t *s_wifiadd_ssid;
static lv_obj_t *s_wifiadd_pass;
static lv_obj_t *s_wifiadd_kb;
static lv_obj_t *s_sys_label;
static lv_obj_t *s_pin_label;            // "PIN: XXXXXXXX" on the 系统 card
static lv_obj_t *s_pin_regen_lbl;        // regen button label (two-tap confirm)
static bool      s_pin_regen_armed;
static lv_obj_t *s_bright_value;
static lv_obj_t *s_nat_sw;               // 穿透/Tunnel enable switch (系统 card)
static lv_obj_t *s_nat_status;           // assigned URL / 未启用 / 连接中

// ----- views: grid / one expanded app / SSH target form / SSH key panel -----
typedef enum { VIEW_GRID, VIEW_APP, VIEW_FORM, VIEW_KEYS, VIEW_WIFIADD } home_view_t;
enum { TA_NAME, TA_HOST, TA_PORT, TA_USER, TA_PASS, TA_CMD, TA_COUNT };

static home_view_t s_view = VIEW_GRID;
static int s_app = APP_SSH;              // expanded app while VIEW_APP/FORM
static lv_obj_t *s_form;                 // form overlay (cream, opaque)
static lv_obj_t *s_form_title;
static lv_obj_t *s_ta[TA_COUNT];
static lv_obj_t *s_os_btn[TARGET_OS_COUNT];
static lv_obj_t *s_auth_btn[TARGET_AUTH_COUNT];
static lv_obj_t *s_del_btn;
static lv_obj_t *s_del_lbl;
static lv_obj_t *s_kb;                   // on-screen lv_keyboard (docked)
static int s_focus;                      // index into s_ta[]
static int s_edit_idx = -1;              // target being edited; -1 = add
static int s_form_os = TARGET_OS_SERVER; // OS picker selection
static int s_form_auth = TARGET_AUTH_AUTO; // auth picker selection
static bool s_del_armed;                 // inline delete confirm state

// ----- SSH key panel (VIEW_KEYS) -----
static lv_obj_t *s_keys;                 // overlay (cream, opaque)
static lv_obj_t *s_key_chip;             // "密钥" button on the SSH card
static lv_obj_t *s_key_status_cjk;       // 已就绪/生成中…/无密钥
static lv_obj_t *s_key_status_lat;       // "RSA-2048"
static lv_obj_t *s_key_pub_ta;           // read-only public-line textarea
static lv_obj_t *s_key_regen_btn, *s_key_regen_lbl;
static lv_obj_t *s_key_del_btn,   *s_key_del_lbl;
static bool s_key_regen_armed, s_key_del_armed;

static bool s_open;

static const lv_font_t *cjk_font(void)
{
    const lv_font_t *f = assets_font_cjk();
    return f ? f : &cjk24;
}

// montserrat has no CJK glyphs; the CJK fonts may lack latin — pick per text.
static const lv_font_t *font_for(const char *txt)
{
    return ((unsigned char)txt[0] >= 0x80) ? cjk_font() : &lv_font_montserrat_14;
}

static void rebuild_tiles(void);
static void refresh_targets(void);
static void apply_view(void);
static bool ssh_connect_allowed(void);
static void ui_home_rebuild(void);
static void refresh_nat(void);

// Language segmented toggle (Display card): two buttons 中文 / English.
static lv_obj_t *s_lang_btn[LANG_COUNT];

static void lang_paint(void)
{
    for (int i = 0; i < LANG_COUNT; i++) {
        if (!s_lang_btn[i]) continue;
        bool sel = (i == (int)i18n_lang());
        lv_obj_set_style_bg_color(s_lang_btn[i],
            lv_color_hex(sel ? 0x4a3500 : 0xC9A35B), 0);
    }
}

static void lang_btn_cb(lv_event_t *e)
{
    lang_t lang = (lang_t)(intptr_t)lv_event_get_user_data(e);
    if (lang == i18n_lang()) return;
    i18n_set_lang(lang);
    s_cfg->lang = (uint8_t)lang;
    settings_save(s_cfg);               // LVGL task = internal stack, NVS-safe
    // Live rebuild: tear down and recreate the whole panel so every static
    // label picks up the new language (and the right font via font_for).
    ui_home_rebuild();
}

// ---------------------------------------------------------------- refresh

static void refresh_clock_batt(void)
{
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char buf[8];
    if (tm_local.tm_year + 1900 < 2025) {
        strcpy(buf, "--:--");
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    }
    lv_label_set_text(s_clock_label, buf);

    float volts;
    int pct;
    bool chg;
    if (power_mon_get(&volts, &pct, &chg)) {
        char bb[24];
        snprintf(bb, sizeof(bb), "%d%%%s", pct, chg ? " " LV_SYMBOL_CHARGE : "");
        lv_label_set_text(s_batt_label, bb);
    } else {
        lv_label_set_text(s_batt_label, "");
    }
    lv_obj_align_to(s_batt_label, s_clock_label, LV_ALIGN_OUT_LEFT_MID, -16, 0);
}

// ----------------------------------------------------------- toast overlay
// Transient centered message on the panel; auto-dismisses. Used to explain
// why an action (e.g. SSH connect) was refused. Runs on the LVGL thread.
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_dismiss_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    s_toast_timer = NULL;
}

static void ui_home_toast(const char *msg)
{
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }

    s_toast = lv_obj_create(s_root);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, 18, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_all(s_toast, 20, 0);
    lv_obj_set_style_shadow_width(s_toast, 24, 0);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_30, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_font(l, font_for(msg), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFBF3E3), 0);
    lv_obj_center(l);

    lv_obj_update_layout(s_toast);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 0);
    s_toast_timer = lv_timer_create(toast_dismiss_cb, 2500, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

// ------------------------------------------------ top-bar Wi-Fi indicator
// Three states. Connected: RSSI-graded green/amber/red glyph + IP text.
// Connecting (creds set, no link yet): amber glyph pulsing via a 300ms timer
// + "连接中". Disconnected: greyed glyph + "未连接". Re-aligned each refresh
// so it sits just left of the battery/clock cluster regardless of IP width.
static void wifi_pulse_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_wifi_top_icon) return;
    // pulse the amber glyph between bright and dim
    s_wifi_pulse_phase ^= 1;
    lv_obj_set_style_text_color(s_wifi_top_icon,
        lv_color_hex(s_wifi_pulse_phase ? 0xC9A35B : 0x7d6c3f), 0);
}

static void wifi_pulse_stop(void)
{
    if (s_wifi_pulse_timer) { lv_timer_delete(s_wifi_pulse_timer); s_wifi_pulse_timer = NULL; }
}

static void refresh_wifi_topbar(void)
{
    if (!s_wifi_top_icon) return;

    bool connected = wifi_is_connected();
    char ip[20];
    bool have_ip = wifi_get_ip(ip, sizeof(ip));
    bool have_creds = s_cfg && s_cfg->wifi_ssid[0];

    if (connected && have_ip) {
        wifi_pulse_stop();
        wifi_ap_record_t ap;
        int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : -100;
        // RSSI tiers: strong >= -60 green, medium >= -75 amber, weak red.
        uint32_t col = rssi >= -60 ? 0x4CAF7D : rssi >= -75 ? 0xC9A35B : 0xB5654B;
        lv_obj_set_style_text_color(s_wifi_top_icon, lv_color_hex(col), 0);
        lv_label_set_text(s_wifi_top_text, ip);
        lv_obj_set_style_text_color(s_wifi_top_text, lv_color_hex(COL_INK), 0);
    } else if (have_creds) {
        // connecting: amber + pulse timer + 连接中
        if (!s_wifi_pulse_timer) {
            s_wifi_pulse_phase = 1;
            s_wifi_pulse_timer = lv_timer_create(wifi_pulse_cb, 300, NULL);
        }
        lv_label_set_text(s_wifi_top_text, T(T_CONNECTING));
        lv_obj_set_style_text_font(s_wifi_top_text, font_for(T(T_CONNECTING)), 0);
        lv_obj_set_style_text_color(s_wifi_top_text, lv_color_hex(0x8d7a4a), 0);
    } else {
        wifi_pulse_stop();
        lv_obj_set_style_text_color(s_wifi_top_icon, lv_color_hex(0x9aa3a8), 0);
        lv_label_set_text(s_wifi_top_text, T(T_DISCONNECTED));
        lv_obj_set_style_text_font(s_wifi_top_text, font_for(T(T_DISCONNECTED)), 0);
        lv_obj_set_style_text_color(s_wifi_top_text, lv_color_hex(0x8d8a7e), 0);
    }
    if (connected && have_ip) {
        lv_obj_set_style_text_font(s_wifi_top_text, &lv_font_montserrat_14, 0);
    }
    // keep the cluster left of the battery (which sits left of the clock)
    lv_obj_align_to(s_wifi_top_text, s_wifi_top_icon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

static void session_row_cb(lv_event_t *e)
{
    ssh_set_active((int)(intptr_t)lv_event_get_user_data(e));
    ui_home_set_open(false);
}

static void session_close_cb(lv_event_t *e)
{
    ssh_close((int)(intptr_t)lv_event_get_user_data(e));
    refresh_targets();
}

// Gate opening a *new* SSH session on Wi-Fi: without a link the connect would
// hang/fail. Switching to an already-open session is fine (no gate). Returns
// true if the caller may proceed; otherwise shows a toast and returns false.
static bool ssh_connect_allowed(void)
{
    if (wifi_is_connected()) return true;
    ui_home_toast(T(T_WIFI_NOT_CONNECTED));
    return false;
}

static void target_row_cb(lv_event_t *e)
{
    if (!ssh_connect_allowed()) return;
    ssh_client_request_switch((int)(intptr_t)lv_event_get_user_data(e));
    ui_home_set_open(false);
}

// Common row chrome for both session and target rows (grid-mode SSH card).
// Rows are passive previews: clicks must bubble to the card so the tap
// expands the SSH app (interaction lives in the expanded tile view).
static lv_obj_t *mk_ssh_row(uint32_t bg, lv_event_cb_t cb, int arg,
                            uint32_t dot_color, const char *name)
{
    (void)cb; (void)arg;
    lv_obj_t *row = lv_obj_create(s_ssh_list);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(bg), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_label_create(row);
    lv_label_set_text(dot, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(dot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, font_for(name), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 28, 0);
    return row;
}

// Rebuild the grid-mode SSH card rows: open sessions first (tap = switch,
// ✕ = close), then the not-yet-open targets (tap = open a new session).

// ------------------------------------------------- device action sheet
// No hidden gestures: tapping a device (mini cell or big tile) opens an
// explicit modal asking what to do — 连接 / 编辑 / 取消.

static lv_obj_t *s_sheet;            // scrim + dialog, created on demand
static int s_sheet_target = -1;

static void sheet_close(void)
{
    if (s_sheet) { lv_obj_delete(s_sheet); s_sheet = NULL; }
    s_sheet_target = -1;
}

static void sheet_cancel_cb(lv_event_t *e) { sheet_close(); }

static void sheet_connect_cb(lv_event_t *e)
{
    int t = s_sheet_target;
    sheet_close();
    if (t < 0) return;
    if (!ssh_connect_allowed()) return;
    ssh_client_request_switch(t);
    ui_home_set_open(false);         // land in the terminal
}

static void open_form(int idx);

static void sheet_edit_cb(lv_event_t *e)
{
    int t = s_sheet_target;
    sheet_close();
    if (t >= 0) open_form(t);
}

static lv_obj_t *sheet_btn(lv_obj_t *par, const char *latin, const char *cjk,
                           uint32_t bg, lv_event_cb_t cb, int x)
{
    lv_obj_t *btn = lv_button_create(par);
    lv_obj_set_size(btn, 130, 52);
    lv_obj_set_pos(btn, x, 120);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, cjk);
    lv_obj_set_style_text_font(l, font_for(cjk), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    (void)latin;
    return btn;
}

static void sheet_show(int target_idx)
{
    if (!s_cfg || target_idx < 0 || target_idx >= s_cfg->n_targets) return;
    sheet_close();
    s_sheet_target = target_idx;

    // full-panel scrim: swallows stray taps, tap = cancel
    s_sheet = lv_obj_create(s_root);
    lv_obj_set_size(s_sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_sheet, 0, 0);
    lv_obj_set_style_bg_color(s_sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sheet, sheet_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dlg = lv_obj_create(s_sheet);
    lv_obj_set_size(dlg, 460, 200);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0xFBF3E3), 0);
    lv_obj_set_style_radius(dlg, 24, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_CLICKABLE);   // eat clicks: don't cancel
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_event_cb(dlg, NULL);

    const ssh_target_t *t = &s_cfg->targets[target_idx];
    int os = s_cfg->target_os[target_idx];
    if (os >= TARGET_OS_COUNT) os = 0;

    lv_obj_t *icon = mk_os_icon(dlg, os, 32, &nerd32, 0x2B2D42);
    lv_obj_set_pos(icon, 0, 4);

    lv_obj_t *nm = lv_label_create(dlg);
    lv_label_set_text(nm, t->name[0] ? t->name : t->host);
    lv_obj_set_style_text_font(nm, font_for(t->name), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0x2B2D42), 0);
    lv_obj_set_pos(nm, 48, 8);

    char addr[88];
    snprintf(addr, sizeof(addr), "%s@%s:%d", t->user, t->host, t->port);
    lv_obj_t *al = lv_label_create(dlg);
    lv_label_set_text(al, addr);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(0x8d8a7e), 0);
    lv_obj_set_pos(al, 0, 56);

    sheet_btn(dlg, "connect", T(T_CONNECT), COL_GREEN, sheet_connect_cb, 0);
    sheet_btn(dlg, "edit",    T(T_EDIT),    COL_NAVY, sheet_edit_cb, 146);
    sheet_btn(dlg, "cancel",  T(T_CANCEL),  0x8d8a7e, sheet_cancel_cb, 292);
}

static void device_tap_cb(lv_event_t *e)
{
    sheet_show((int)(intptr_t)lv_event_get_user_data(e));
}


static void refresh_targets(void)
{
    lv_obj_clean(s_ssh_list);

    // Grid-card preview: small icon+name cells, 3x2 = up to 6; if there are
    // more targets the last cell becomes a "+N" overflow marker. Pure
    // preview — everything is non-clickable so the tap expands the app.
    bool target_open[MAX_SSH_TARGETS] = {0};
    uint32_t target_dot[MAX_SSH_TARGETS];
    for (int i = 0; i < MAX_SSH_TARGETS; i++) target_dot[i] = 0x6b7689;

    for (int id = 0; id < MAX_SSH_SESSIONS; id++) {
        char name[48];
        int tgt;
        ssh_state_t st = ssh_state(id, name, sizeof(name), &tgt);
        if (tgt < 0 || tgt >= MAX_SSH_TARGETS) continue;
        target_open[tgt] = true;
        target_dot[tgt] = (st == SSH_STATE_CONNECTED)  ? 0x7CFC9A
                        : (st == SSH_STATE_CONNECTING) ? 0xFFD23D : 0x9aa3b5;
    }

    // open targets first, then the rest
    int order[MAX_SSH_TARGETS], n = 0;
    for (int i = 0; i < s_cfg->n_targets; i++) if (target_open[i]) order[n++] = i;
    for (int i = 0; i < s_cfg->n_targets; i++) if (!target_open[i]) order[n++] = i;

    const int max_cells = 6;
    int shown = n <= max_cells ? n : max_cells - 1;

    for (int k = 0; k < shown; k++) {
        const ssh_target_t *t = &s_cfg->targets[order[k]];
        int os = s_cfg->target_os[order[k]];
        if (os >= TARGET_OS_COUNT) os = 0;

        lv_obj_t *cell = lv_obj_create(s_ssh_list);
        lv_obj_set_size(cell, (CARD_W - 40 - 16) / 3, 96);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x3b4a60), 0);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        // No hidden gestures: a tap opens the explicit 连接/编辑 sheet.
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, device_tap_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)order[k]);

        lv_obj_t *icon = mk_os_icon(cell, os, 32, &nerd32, 0xdce4f2);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);

        lv_obj_t *dot = lv_obj_create(cell);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(target_dot[order[k]]), 0);
        lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -2, 2);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *nm = lv_label_create(cell);
        lv_label_set_text(nm, t->name[0] ? t->name : t->host);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xb8c2d6), 0);
        lv_obj_set_width(nm, LV_PCT(100));
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    if (n > max_cells) {                        // overflow marker cell
        lv_obj_t *cell = lv_obj_create(s_ssh_list);
        lv_obj_set_size(cell, (CARD_W - 40 - 16) / 3, 96);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x2e3a4c), 0);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *m = lv_label_create(cell);
        char more[16];
        snprintf(more, sizeof(more), "+%d", n - shown);
        lv_label_set_text(m, more);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(0xb8c2d6), 0);
        lv_obj_center(m);
    }

    if (s_cfg->n_targets == 0) {
        lv_obj_t *lbl = lv_label_create(s_ssh_list);
        lv_label_set_text(lbl, "no targets - tap to add");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xb8c2d6), 0);
    }

    char sub[24];
    snprintf(sub, sizeof(sub), "%d open / %d", ssh_session_count(), s_cfg->n_targets);
    lv_label_set_text(s_ssh_sub, sub);

    if (s_view == VIEW_APP && s_app == APP_SSH) rebuild_tiles();
}

static void rebuild_wifi_list(void);

static void refresh_wifi(void)
{
    wifi_ap_record_t ap;
    char ip[20];
    bool conn = esp_wifi_sta_get_ap_info(&ap) == ESP_OK
                && wifi_get_ip(ip, sizeof(ip));

    // Grid face: just the connected SSID (or "Wi-Fi"), no status lines.
    lv_label_set_text(s_wifi_face_name,
                      conn ? (const char *)ap.ssid : "Wi-Fi");

    // Expanded detail: full current-connection block.
    if (s_wifi_cur_label) {
        char buf[120];
        if (conn) {
            snprintf(buf, sizeof(buf), "%s\n%d dBm   %s",
                     (const char *)ap.ssid, ap.rssi, ip);
        } else {
            strlcpy(buf, T(T_DISCONNECTED), sizeof(buf));
        }
        lv_label_set_text(s_wifi_cur_label, buf);
        lv_obj_set_style_text_font(s_wifi_cur_label,
                                   conn ? &lv_font_montserrat_14 : font_for(buf), 0);
    }
    if (s_wifi_list) rebuild_wifi_list();
}

static void refresh_ble(void)
{
    bool en  = ble_prov_is_enabled();
    bool adv = ble_prov_is_advertising();
    if (s_ble_state_label) {
        const char *st = !en  ? T(T_BT_OFF)
                        : adv ? T(T_ADVERTISING) : T(T_ADV_STOPPED);
        lv_label_set_text(s_ble_state_label, st);
        lv_obj_set_style_text_font(s_ble_state_label, font_for(st), 0);
    }
    if (s_ble_btn_label) {
        const char *bt = adv ? T(T_STOP_ADV) : T(T_START_ADV);
        lv_label_set_text(s_ble_btn_label, bt);
        lv_obj_set_style_text_font(s_ble_btn_label, font_for(bt), 0);
    }
    // master switch: reflect state without re-firing the value-changed cb
    if (s_ble_master_btn) {
        if (en) lv_obj_add_state(s_ble_master_btn, LV_STATE_CHECKED);
        else    lv_obj_clear_state(s_ble_master_btn, LV_STATE_CHECKED);
    }
    // Adv toggle is meaningless while the stack is disabled.
    if (s_ble_btn) {
        if (en) lv_obj_clear_state(s_ble_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(s_ble_btn, LV_STATE_DISABLED);
    }
}

// ----------------------------------------------------- 连接: Wi-Fi saved nets
// Each saved network is a row: name + 连接 / 忘记 buttons. Connecting copies
// the cred into the active pair and reconnects on a background task; forgetting
// removes it from the array. Both persist via settings_save (LVGL task =
// internal stack, NVS-safe).

static void wifi_connect_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    wifi_connect_saved_async(s_cfg, idx);   // copies active pair + save + task
    refresh_wifi();
}

static void wifi_forget_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_cfg->n_wifi_nets) return;
    memmove(&s_cfg->wifi_nets[idx], &s_cfg->wifi_nets[idx + 1],
            (s_cfg->n_wifi_nets - idx - 1) * sizeof(wifi_cred_t));
    s_cfg->n_wifi_nets--;
    settings_save(s_cfg);
    rebuild_wifi_list();
}

static void wifiadd_open(void);

static void wifi_add_cb(lv_event_t *e) { wifiadd_open(); }

static void rebuild_wifi_list(void)
{
    if (!s_wifi_list) return;
    lv_obj_clean(s_wifi_list);

    for (int i = 0; i < s_cfg->n_wifi_nets; i++) {
        bool active = strcmp(s_cfg->wifi_nets[i].ssid, s_cfg->wifi_ssid) == 0;
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(active ? 0x1f6b54 : 0x2a8a93), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, s_cfg->wifi_nets[i].ssid);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xffffff), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *fbtn = lv_button_create(row);   // 忘记
        lv_obj_set_size(fbtn, 70, 36);
        lv_obj_align(fbtn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(fbtn, lv_color_hex(0x9c3a28), 0);
        lv_obj_set_style_radius(fbtn, 10, 0);
        lv_obj_set_style_shadow_width(fbtn, 0, 0);
        lv_obj_add_event_cb(fbtn, wifi_forget_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *fl = lv_label_create(fbtn);
        lv_label_set_text(fl, T(T_FORGET));
        lv_obj_set_style_text_font(fl, font_for(T(T_FORGET)), 0);
        lv_obj_set_style_text_color(fl, lv_color_hex(0xffffff), 0);
        lv_obj_center(fl);

        lv_obj_t *cbtn = lv_button_create(row);   // 连接
        lv_obj_set_size(cbtn, 70, 36);
        lv_obj_align(cbtn, LV_ALIGN_RIGHT_MID, -78, 0);
        lv_obj_set_style_bg_color(cbtn, lv_color_hex(active ? 0x294d3a : COL_GREEN), 0);
        lv_obj_set_style_radius(cbtn, 10, 0);
        lv_obj_set_style_shadow_width(cbtn, 0, 0);
        lv_obj_add_event_cb(cbtn, wifi_connect_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *cl = lv_label_create(cbtn);
        const char *clt = active ? T(T_CONNECTED) : T(T_CONNECT);
        lv_label_set_text(cl, clt);
        lv_obj_set_style_text_font(cl, font_for(clt), 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
        lv_obj_center(cl);
    }

    if (s_cfg->n_wifi_nets == 0) {
        lv_obj_t *l = lv_label_create(s_wifi_list);
        lv_label_set_text(l, T(T_NO_SAVED_NETS));
        lv_obj_set_style_text_font(l, font_for(T(T_NO_SAVED_NETS)), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xcfeef0), 0);
    }
}

// --------------------------------------------------- 连接: 添加网络 mini-form
// SSID + password textareas over the expanded 连接 app. Same keyboard plumbing
// as the SSH form: hw keys via ui_home_handle_key (VIEW_WIFIADD), on-screen
// lv_keyboard only when no hw keyboard is present. Saving appends to wifi_nets
// and connects to it.

static lv_obj_t *s_wifiadd_focus_ta;     // textarea the on-screen kb drives
static lv_obj_t *s_scan_list;            // SSID scan results panel
static lv_obj_t *s_scan_btn_label;
static volatile bool s_scanning;
// Scan results handed from the scan task to the LVGL thread via lv_async_call.
#define SCAN_MAX 20
static wifi_ap_record_t s_scan_recs[SCAN_MAX];
static volatile int s_scan_n;

static void wifiadd_close(void)
{
    s_view = VIEW_APP;
    s_app = APP_CONN;
    apply_view();
}

static void wifiadd_save_cb(lv_event_t *e)
{
    const char *ssid = lv_textarea_get_text(s_wifiadd_ssid);
    if (!ssid[0]) {
        lv_obj_set_style_border_color(s_wifiadd_ssid, lv_color_hex(0xD64545), 0);
        return;
    }
    if (s_cfg->n_wifi_nets >= MAX_WIFI_NETS) { wifiadd_close(); return; }
    int idx = s_cfg->n_wifi_nets++;
    memset(&s_cfg->wifi_nets[idx], 0, sizeof(wifi_cred_t));
    strlcpy(s_cfg->wifi_nets[idx].ssid, ssid, sizeof(s_cfg->wifi_nets[idx].ssid));
    strlcpy(s_cfg->wifi_nets[idx].pass, lv_textarea_get_text(s_wifiadd_pass),
            sizeof(s_cfg->wifi_nets[idx].pass));
    // Saving connects to it (copies active pair + save + background reconnect).
    wifi_connect_saved_async(s_cfg, idx);
    rebuild_wifi_list();
    wifiadd_close();
}

static void wifiadd_cancel_cb(lv_event_t *e) { wifiadd_close(); }

static void wifiadd_ta_focus_cb(lv_event_t *e)
{
    s_wifiadd_focus_ta = lv_event_get_target(e);
    if (s_wifiadd_kb) lv_keyboard_set_textarea(s_wifiadd_kb, s_wifiadd_focus_ta);
}

static void scan_row_free_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void scan_fill_ssid_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    lv_textarea_set_text(s_wifiadd_ssid, ssid);
    lv_obj_set_style_border_color(s_wifiadd_ssid, lv_color_hex(0xD8CFAE), 0);
}

// Runs on the LVGL thread (via lv_async_call) once the scan task finishes.
static void scan_results_ready(void *unused)
{
    (void)unused;
    s_scanning = false;
    if (s_scan_btn_label) {
        lv_label_set_text(s_scan_btn_label, T(T_SCAN));
        lv_obj_set_style_text_font(s_scan_btn_label, font_for(T(T_SCAN)), 0);
    }
    if (!s_scan_list) return;
    lv_obj_clean(s_scan_list);

    if (s_scan_n <= 0) {
        lv_obj_t *l = lv_label_create(s_scan_list);
        lv_label_set_text(l, T(T_NO_NETS));
        lv_obj_set_style_text_font(l, font_for(T(T_NO_NETS)), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_INK2), 0);
        return;
    }
    // de-dup by SSID (strongest kept; wifi_scan already sorts by RSSI desc)
    char seen[SCAN_MAX][33];
    int nseen = 0;
    for (int i = 0; i < s_scan_n && i < SCAN_MAX; i++) {
        const char *ssid = (const char *)s_scan_recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < nseen; j++) if (!strcmp(seen[j], ssid)) { dup = true; break; }
        if (dup) continue;
        strlcpy(seen[nseen++], ssid, 33);

        lv_obj_t *row = lv_obj_create(s_scan_list);
        lv_obj_set_size(row, LV_PCT(100), 44);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a8a93), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // user_data points into seen[]; copy to a stable per-row strdup
        char *sd = lv_malloc(33);
        strlcpy(sd, ssid, 33);
        lv_obj_add_event_cb(row, scan_fill_ssid_cb, LV_EVENT_CLICKED, sd);
        lv_obj_add_event_cb(row, scan_row_free_cb, LV_EVENT_DELETE, sd);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, ssid);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xffffff), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        char meta[24];
        snprintf(meta, sizeof(meta), "%d dBm", s_scan_recs[i].rssi);
        lv_obj_t *m = lv_label_create(row);
        lv_label_set_text(m, meta);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(m, lv_color_hex(0xcfe8ea), 0);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

// Background task: blocking wifi_scan, then marshal back to the LVGL thread.
// wifi only (no flash) so an internal-stack task is fine.
static void scan_task(void *arg)
{
    int n = wifi_scan(s_scan_recs, SCAN_MAX);
    s_scan_n = n < 0 ? 0 : n;
    // lv_async_call mutates LVGL state — must hold the LVGL lock from a
    // non-LVGL task (esp_lvgl_port lock is recursive).
    bsp_display_lock(0);
    lv_async_call(scan_results_ready, NULL);
    bsp_display_unlock();
    vTaskDelete(NULL);
}

static void scan_btn_cb(lv_event_t *e)
{
    if (s_scanning) return;
    s_scanning = true;
    if (s_scan_btn_label) {
        lv_label_set_text(s_scan_btn_label, T(T_SCANNING));
        lv_obj_set_style_text_font(s_scan_btn_label, font_for(T(T_SCANNING)), 0);
    }
    if (s_scan_list) {
        lv_obj_clean(s_scan_list);
        lv_obj_t *l = lv_label_create(s_scan_list);
        lv_label_set_text(l, T(T_SCANNING));
        lv_obj_set_style_text_font(l, font_for(T(T_SCANNING)), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_INK2), 0);
    }
    xTaskCreate(scan_task, "wifiscan", 4096, NULL, 4, NULL);
}

static void mk_wifiadd(void)
{
    s_wifiadd = lv_obj_create(s_root);
    lv_obj_set_size(s_wifiadd, APP_W, APP_H);
    lv_obj_set_pos(s_wifiadd, MARGIN_X, APP_Y);
    lv_obj_set_style_bg_color(s_wifiadd, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(s_wifiadd, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_wifiadd, 24, 0);
    lv_obj_set_style_border_width(s_wifiadd, 0, 0);
    lv_obj_set_style_pad_all(s_wifiadd, 24, 0);
    lv_obj_clear_flag(s_wifiadd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifiadd, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_wifiadd);
    lv_label_set_text(title, T(T_ADD_NET));
    lv_obj_set_style_text_font(title, font_for(T(T_ADD_NET)), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_INK2), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *sl = lv_label_create(s_wifiadd);
    lv_label_set_text(sl, T(T_NAME));
    lv_obj_set_style_text_font(sl, font_for(T(T_NAME)), 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(sl, 0, 64);
    s_wifiadd_ssid = lv_textarea_create(s_wifiadd);
    lv_textarea_set_one_line(s_wifiadd_ssid, true);
    lv_textarea_set_max_length(s_wifiadd_ssid, 32);
    lv_obj_set_size(s_wifiadd_ssid, 340, 46);
    lv_obj_set_pos(s_wifiadd_ssid, 92, 56);
    lv_obj_set_style_radius(s_wifiadd_ssid, 10, 0);
    lv_obj_set_style_text_font(s_wifiadd_ssid, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_wifiadd_ssid, wifiadd_ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *scanb = lv_button_create(s_wifiadd);    // SSID scan
    lv_obj_set_size(scanb, 100, 46);
    lv_obj_set_pos(scanb, 444, 56);
    lv_obj_set_style_bg_color(scanb, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_radius(scanb, 10, 0);
    lv_obj_set_style_shadow_width(scanb, 0, 0);
    lv_obj_add_event_cb(scanb, scan_btn_cb, LV_EVENT_CLICKED, NULL);
    s_scan_btn_label = lv_label_create(scanb);
    lv_label_set_text(s_scan_btn_label, T(T_SCAN));
    lv_obj_set_style_text_font(s_scan_btn_label, font_for(T(T_SCAN)), 0);
    lv_obj_set_style_text_color(s_scan_btn_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_scan_btn_label);

    // scan results panel (right side), tap a row to fill the SSID field
    lv_obj_t *scanhdr = lv_label_create(s_wifiadd);
    lv_label_set_text(scanhdr, T(T_NEARBY_NETS));
    lv_obj_set_style_text_font(scanhdr, font_for(T(T_NEARBY_NETS)), 0);
    lv_obj_set_style_text_color(scanhdr, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(scanhdr, 600, 4);
    s_scan_list = lv_obj_create(s_wifiadd);
    lv_obj_set_size(s_scan_list, APP_W - 48 - 600, 240);
    lv_obj_set_pos(s_scan_list, 600, 36);
    lv_obj_set_style_bg_color(s_scan_list, lv_color_hex(0x1d6f78), 0);
    lv_obj_set_style_bg_opa(s_scan_list, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_scan_list, 12, 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);
    lv_obj_set_style_pad_all(s_scan_list, 8, 0);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scan_list, 6, 0);

    lv_obj_t *pl = lv_label_create(s_wifiadd);
    lv_label_set_text(pl, T(T_PASSWORD));
    lv_obj_set_style_text_font(pl, font_for(T(T_PASSWORD)), 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(pl, 0, 126);
    s_wifiadd_pass = lv_textarea_create(s_wifiadd);
    lv_textarea_set_one_line(s_wifiadd_pass, true);
    lv_textarea_set_password_mode(s_wifiadd_pass, true);
    lv_textarea_set_max_length(s_wifiadd_pass, 64);
    lv_obj_set_size(s_wifiadd_pass, 460, 46);
    lv_obj_set_pos(s_wifiadd_pass, 92, 118);
    lv_obj_set_style_radius(s_wifiadd_pass, 10, 0);
    lv_obj_set_style_text_font(s_wifiadd_pass, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(s_wifiadd_pass, wifiadd_ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *save = lv_button_create(s_wifiadd);
    lv_obj_set_size(save, 140, 52);
    lv_obj_set_pos(save, 92, 184);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x4CAF7D), 0);
    lv_obj_set_style_radius(save, 16, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, wifiadd_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(save);
    lv_label_set_text(svl, T(T_SAVE_CONNECT));
    lv_obj_set_style_text_font(svl, font_for(T(T_SAVE_CONNECT)), 0);
    lv_obj_set_style_text_color(svl, lv_color_hex(0xffffff), 0);
    lv_obj_center(svl);

    lv_obj_t *cancel = lv_button_create(s_wifiadd);
    lv_obj_set_size(cancel, 120, 52);
    lv_obj_set_pos(cancel, 244, 184);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x6b6856), 0);
    lv_obj_set_style_radius(cancel, 16, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, wifiadd_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cnl = lv_label_create(cancel);
    lv_label_set_text(cnl, T(T_CANCEL));
    lv_obj_set_style_text_font(cnl, font_for(T(T_CANCEL)), 0);
    lv_obj_set_style_text_color(cnl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cnl);

    s_wifiadd_kb = lv_keyboard_create(s_wifiadd);
    lv_keyboard_set_mode(s_wifiadd_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(s_wifiadd_kb, APP_W - 48, 280);
    lv_obj_align(s_wifiadd_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void wifiadd_open(void)
{
    if (!s_wifiadd) mk_wifiadd();
    lv_textarea_set_text(s_wifiadd_ssid, "");
    lv_textarea_set_text(s_wifiadd_pass, "");
    if (s_scan_list) lv_obj_clean(s_scan_list);
    lv_obj_set_style_border_color(s_wifiadd_ssid, lv_color_hex(0xD8CFAE), 0);
    s_wifiadd_focus_ta = s_wifiadd_ssid;
    // Hide the on-screen keyboard when a hardware keyboard is present.
    if (hid_keyboard_hw_present()) lv_obj_add_flag(s_wifiadd_kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_wifiadd_kb, LV_OBJ_FLAG_HIDDEN);
    s_app = APP_CONN;
    s_view = VIEW_WIFIADD;
    apply_view();
    lv_obj_add_state(s_wifiadd_ssid, LV_STATE_FOCUSED);
    if (s_wifiadd_kb) lv_keyboard_set_textarea(s_wifiadd_kb, s_wifiadd_ssid);
}

// SSH key panel: status + public line. Cheap; called from the 1 s timer
// while the panel is showing so 生成中… flips to 已就绪 by itself.
static void refresh_keys(void)
{
    if (!s_keys || lv_obj_has_flag(s_keys, LV_OBJ_FLAG_HIDDEN)) return;
    ssh_keys_status_t ks = ssh_keys_status();
    const char *kst = ks == SSH_KEYS_READY ? T(T_KEY_READY)
                      : ks == SSH_KEYS_GENERATING ? T(T_KEY_GENERATING) : T(T_NO_KEY);
    lv_label_set_text(s_key_status_cjk, kst);
    lv_obj_set_style_text_font(s_key_status_cjk, font_for(kst), 0);
    lv_label_set_text(s_key_status_lat, ks == SSH_KEYS_READY ? "RSA-2048" : "");
    lv_obj_align_to(s_key_status_lat, s_key_status_cjk,
                    LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    char line[640];
    if (!ssh_keys_public_line(line, sizeof(line))) {
        strcpy(line, ks == SSH_KEYS_GENERATING ? "(generating...)" : "(no key)");
    }
    if (strcmp(lv_textarea_get_text(s_key_pub_ta), line) != 0) {
        lv_textarea_set_text(s_key_pub_ta, line);
    }
}

static void pin_regen_reset(void)
{
    s_pin_regen_armed = false;
    if (s_pin_regen_lbl) {
        lv_label_set_text(s_pin_regen_lbl, T(T_REGEN_PIN));
        lv_obj_set_style_text_font(s_pin_regen_lbl, font_for(T(T_REGEN_PIN)), 0);
    }
}

static void pin_regen_cb(lv_event_t *e)   // two-tap confirm -> new PIN
{
    (void)e;
    if (!s_pin_regen_armed) {
        s_pin_regen_armed = true;
        lv_label_set_text(s_pin_regen_lbl, T(T_CONFIRM_REGEN));
        lv_obj_set_style_text_font(s_pin_regen_lbl, font_for(T(T_CONFIRM_REGEN)), 0);
        return;
    }
    security_regen_pin();
    if (s_pin_label) {
        char buf[24];
        snprintf(buf, sizeof(buf), "PIN: %s", security_pin());
        lv_label_set_text(s_pin_label, buf);
    }
    pin_regen_reset();
}

// 穿透/Tunnel enable switch: persists nat_enabled and starts/stops the dialer.
// settings_save runs on the LVGL task (internal stack) so the NVS write is safe.
static void nat_sw_cb(lv_event_t *e)
{
    bool en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_cfg->nat_enabled = en;
    settings_save(s_cfg);
    if (en) nat_tunnel_start();
    else    nat_tunnel_stop();
    refresh_nat();
}

static void refresh_sys(void)
{
    if (s_pin_label) {
        char pbuf[24];
        snprintf(pbuf, sizeof(pbuf), "PIN: %s", security_pin());
        lv_label_set_text(s_pin_label, pbuf);
    }
    const esp_app_desc_t *d = esp_app_get_description();
    uint64_t up = esp_timer_get_time() / 1000000ULL;
    float c = 0;
    bool have_temp = status_bar_get_temp(&c);
    char buf[192];
    int len = snprintf(buf, sizeof(buf), "%s %s\nup %llud %02llu:%02llu",
                       d->project_name, d->version,
                       up / 86400, (up / 3600) % 24, (up / 60) % 60);
    if (have_temp) {
        len += snprintf(buf + len, sizeof(buf) - len, "   %.0f C", c);
    }
    snprintf(buf + len, sizeof(buf) - len, "\nheap %u KB   psram %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(s_sys_label, buf);
}

// 穿透/Tunnel status line on the 系统 card: connected -> the assigned public
// host (ASCII, montserrat); enabled-but-not-yet -> 连接中; off -> 未启用.
// NOTE: the host is ASCII so it never mixes scripts with the CJK status words.
static void refresh_nat(void)
{
    if (!s_nat_status) return;
    char host[64];
    const char *txt;
    if (s_cfg->nat_enabled && nat_tunnel_status(host, sizeof(host)) && host[0]) {
        txt = host;
    } else if (s_cfg->nat_enabled) {
        txt = T(T_TUNNEL_CONNECTING);
    } else {
        txt = T(T_TUNNEL_OFF);
    }
    lv_label_set_text(s_nat_status, txt);
    lv_obj_set_style_text_font(s_nat_status, font_for(txt), 0);
}

// Runs in the LVGL task while the panel is open: clock every second, the
// slower sources every 5th tick (status_bar.c cadence).
static void tick_cb(lv_timer_t *timer)
{
    static int div;
    refresh_clock_batt();
    refresh_wifi_topbar();
    if (div++ % 5 == 0) {
        refresh_targets();
        refresh_wifi();
        refresh_ble();
        refresh_sys();
        refresh_nat();
    }
    refresh_keys();   // no-op unless the key panel is showing
}

static void refresh_all(void)
{
    refresh_clock_batt();
    refresh_wifi_topbar();
    refresh_targets();
    refresh_wifi();
    refresh_ble();
    refresh_sys();
    refresh_nat();
}

// ------------------------------------------------------- device tile grid
// Expanded SSH app: one tile per open session (first) and per remaining
// target, plus a trailing "+" add tile.

static void tile_open_cb(lv_event_t *e)     // tap: open-or-switch + terminal
{
    if (!ssh_connect_allowed()) return;
    ssh_client_request_switch((int)(intptr_t)lv_event_get_user_data(e));
    ui_home_set_open(false);
}

static void tile_session_cb(lv_event_t *e)  // tap on an open session's tile
{
    ssh_set_active((int)(intptr_t)lv_event_get_user_data(e));
    ui_home_set_open(false);
}

static void open_form(int idx);

static void tile_edit_cb(lv_event_t *e)     // long-press: edit form
{
    open_form((int)(intptr_t)lv_event_get_user_data(e));
}

static void tile_add_cb(lv_event_t *e)      { open_form(-1); }

// One device tile: big OS icon, name, user@host, status dot.
static lv_obj_t *mk_tile(int target_idx, const char *name, const char *sub,
                         uint32_t dot_color)
{
    const ssh_target_t *t = &s_cfg->targets[target_idx];
    (void)t;
    lv_obj_t *tile = lv_obj_create(s_tiles);
    lv_obj_set_size(tile, 270, 190);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x3b4a60), 0);
    lv_obj_set_style_pad_all(tile, 14, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    // No hidden gestures: a tap opens the 连接/编辑/取消 sheet.
    lv_obj_add_event_cb(tile, device_tap_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)target_idx);

    uint8_t os = s_cfg->target_os[target_idx];
    if (os >= TARGET_OS_COUNT) os = TARGET_OS_SERVER;
    lv_obj_t *icon = mk_os_icon(tile, os, 64, &nerd64, 0xE8EDF5);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *nl = lv_label_create(tile);
    lv_label_set_text(nl, name);
    lv_obj_set_style_text_font(nl, font_for(name), 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_obj_t *al = lv_label_create(tile);
    lv_label_set_text(al, sub);
    lv_obj_set_style_text_font(al, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(al, lv_color_hex(0xb8c2d6), 0);
    lv_label_set_long_mode(al, LV_LABEL_LONG_DOT);
    lv_obj_set_width(al, LV_PCT(100));
    lv_obj_set_style_text_align(al, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(al, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *dot = lv_obj_create(tile);    // status dot, top-right
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    return tile;
}

static void rebuild_tiles(void)
{
    lv_obj_clean(s_tiles);

    bool target_open[MAX_SSH_TARGETS] = {0};

    // Open sessions first (green = connected, amber = connecting).
    for (int id = 0; id < MAX_SSH_SESSIONS; id++) {
        int tgt = -1;
        ssh_state_t st = ssh_state(id, NULL, 0, &tgt);
        if (tgt < 0 || tgt >= s_cfg->n_targets) continue;
        target_open[tgt] = true;
        const ssh_target_t *t = &s_cfg->targets[tgt];
        char sub[88];
        snprintf(sub, sizeof(sub), "%s@%s", t->user, t->host);
        uint32_t dot = (st == SSH_STATE_CONNECTED) ? COL_GREEN : COL_YELLOW;
        lv_obj_t *tile = mk_tile(tgt, t->name[0] ? t->name : t->host, sub, dot);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x46566e), 0);
    }

    // Then the not-yet-open targets (grey dot).
    for (int i = 0; i < s_cfg->n_targets; i++) {
        if (target_open[i]) continue;
        const ssh_target_t *t = &s_cfg->targets[i];
        char sub[88];
        snprintf(sub, sizeof(sub), "%s@%s", t->user, t->host);
        lv_obj_t *tile = mk_tile(i, t->name[0] ? t->name : t->host, sub, 0x6b7689);
    }

    // Trailing "+" tile (add a target).
    if (s_cfg->n_targets < MAX_SSH_TARGETS) {
        lv_obj_t *tile = lv_obj_create(s_tiles);
        lv_obj_set_size(tile, 270, 190);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x5a6a82), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_add_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *plus = lv_label_create(tile);
        lv_label_set_text(plus, LV_SYMBOL_PLUS);
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(plus, lv_color_hex(0x9aa3b5), 0);
        lv_obj_center(plus);
    }
}

// ----------------------------------------------------- launcher mechanics

static void expand_cb(lv_event_t *e)        // tap a grid card
{
    if (s_view != VIEW_GRID) return;
    s_app = (int)(intptr_t)lv_event_get_user_data(e);
    s_view = VIEW_APP;
    apply_view();
}

static void dock_cb(lv_event_t *e)          // tap a dock icon
{
    s_app = (int)(intptr_t)lv_event_get_user_data(e);
    s_view = VIEW_APP;
    apply_view();
}

static void back_cb(lv_event_t *e)          // dock back button
{
    s_view = VIEW_GRID;
    apply_view();
}

// Grid position for card i: 3 cards in the top row, 2 centered in the
// second row (5 cards since the Wi-Fi/BLE merge).
static void card_grid_pos(int i, int *x, int *y)
{
    if (i < 3) {
        *x = MARGIN_X + i * (CARD_W + GUTTER);
        *y = TOP_H + 12;
    } else {
        *x = MARGIN_X + (CARD_W + GUTTER) / 2 + (i - 3) * (CARD_W + GUTTER);
        *y = TOP_H + 12 + CARD_H + GUTTER;
    }
}

// Lays the cards/dock/form/key-panel out for the current s_view/s_app.
// LVGL lock held.
static void apply_view(void)
{
    bool form = (s_view == VIEW_FORM);
    bool keys = (s_view == VIEW_KEYS);
    bool wadd = (s_view == VIEW_WIFIADD);
    if (s_wifiadd) {
        if (wadd) {
            lv_obj_clear_flag(s_wifiadd, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_wifiadd);
        } else {
            lv_obj_add_flag(s_wifiadd, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (form) {
        lv_obj_clear_flag(s_form, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_form);
    } else {
        lv_obj_add_flag(s_form, LV_OBJ_FLAG_HIDDEN);
        s_del_armed = false;
    }
    if (keys) {
        lv_obj_clear_flag(s_keys, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keys);
        refresh_keys();
    } else {
        lv_obj_add_flag(s_keys, LV_OBJ_FLAG_HIDDEN);
        s_key_regen_armed = s_key_del_armed = false;
    }

    if (s_view == VIEW_GRID) {
        lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < APP_COUNT; i++) {
            int x, y;
            card_grid_pos(i, &x, &y);
            lv_obj_clear_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_card[i], CARD_W, CARD_H);
            lv_obj_set_pos(s_card[i], x, y);
        }
        // SSH card back to grid content
        lv_obj_clear_flag(s_ssh_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ssh_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_manage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tiles, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_key_chip, LV_OBJ_FLAG_HIDDEN);
        // 连接 card: minimal face (icon + name), no expanded detail.
        if (s_conn_face)   lv_obj_clear_flag(s_conn_face, LV_OBJ_FLAG_HIDDEN);
        if (s_conn_detail) lv_obj_add_flag(s_conn_detail, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // VIEW_APP / VIEW_FORM / VIEW_KEYS: one expanded card + dock.
    lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dock);
    if (form) lv_obj_move_foreground(s_form);
    if (keys) lv_obj_move_foreground(s_keys);
    for (int i = 0; i < APP_COUNT; i++) {
        if (i == s_app) {
            lv_obj_clear_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(s_card[i], APP_W, APP_H);
            lv_obj_set_pos(s_card[i], MARGIN_X, APP_Y);
        } else {
            lv_obj_add_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
        }
        // current app's dock icon gets an ink ring
        lv_obj_set_style_border_width(s_dock_btn[i], i == s_app ? 3 : 0, 0);
    }
    // expanded: the whole view IS management — the corner chip is noise
    lv_obj_add_flag(s_manage, LV_OBJ_FLAG_HIDDEN);
    if (s_app == APP_SSH) {
        lv_obj_add_flag(s_ssh_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ssh_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_tiles, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_key_chip, LV_OBJ_FLAG_HIDDEN);
        rebuild_tiles();
    } else {
        lv_obj_clear_flag(s_ssh_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ssh_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tiles, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_key_chip, LV_OBJ_FLAG_HIDDEN);
    }
    // 连接 app: hide the sparse face, show the full detail container.
    if (s_app == APP_CONN) {
        if (s_conn_face)   lv_obj_add_flag(s_conn_face, LV_OBJ_FLAG_HIDDEN);
        if (s_conn_detail) lv_obj_clear_flag(s_conn_detail, LV_OBJ_FLAG_HIDDEN);
        refresh_wifi();
        refresh_ble();
    } else {
        if (s_conn_face)   lv_obj_clear_flag(s_conn_face, LV_OBJ_FLAG_HIDDEN);
        if (s_conn_detail) lv_obj_add_flag(s_conn_detail, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mk_dock(void)
{
    s_dock = lv_obj_create(s_root);
    lv_obj_set_size(s_dock, HOME_W, DOCK_H);
    lv_obj_set_pos(s_dock, 0, HOME_H - DOCK_H);
    lv_obj_set_style_bg_color(s_dock, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dock, 0, 0);
    lv_obj_set_style_radius(s_dock, 0, 0);
    lv_obj_set_style_pad_all(s_dock, 0, 0);
    lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);

    // back-to-grid button, far left
    lv_obj_t *back = lv_button_create(s_dock);
    lv_obj_set_size(back, 64, 64);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, MARGIN_X, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);

    // app icons, centered
    int total = APP_COUNT * 64 + (APP_COUNT - 1) * 18;
    int x0 = (HOME_W - total) / 2;
    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_dock);
        lv_obj_set_size(btn, 64, 64);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, x0 + i * (64 + 18), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(app_color[i]), 0);
        lv_obj_set_style_radius(btn, 20, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_INK), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, dock_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, app_glyph[i]);
        lv_obj_set_style_text_font(l, &nerd32, 0);
        lv_obj_set_style_text_color(l,
            lv_color_hex(i == APP_DISP ? 0x4a3500 : 0xffffff), 0);
        lv_obj_center(l);
        s_dock_btn[i] = btn;
    }
}

// ---------------------------------------------------------------- widgets

static void ble_btn_cb(lv_event_t *e)
{
    if (!ble_prov_is_enabled()) return;           // disabled stack: no-op
    if (ble_prov_is_advertising()) ble_prov_stop();
    else ble_prov_start();
    refresh_ble();
}

static void ble_master_cb(lv_event_t *e)
{
    bool en = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ble_prov_set_enabled(en);
    s_cfg->ble_enabled = en;
    settings_save(s_cfg);                          // LVGL task = internal stack
    refresh_ble();
}

static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    bsp_display_brightness_set(v);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_bright_value, buf);
}

static lv_obj_t *mk_card(int idx)
{
    lv_obj_t *card = lv_obj_create(s_root);
    int x, y;
    card_grid_pos(idx, &x, &y);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(app_color[idx]), 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, expand_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    s_card[idx] = card;
    return card;
}

// Card title: optional latin part (montserrat) + CJK part (CJK font),
// side by side — neither font family covers the other script.
static void mk_title(lv_obj_t *card, const char *latin, const char *cjk, uint32_t color)
{
    lv_obj_t *prev = NULL;
    if (latin) {
        prev = lv_label_create(card);
        lv_label_set_text(prev, latin);
        lv_obj_set_style_text_font(prev, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(prev, lv_color_hex(color), 0);
        lv_obj_align(prev, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    if (cjk) {
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, cjk);
        // CJK -> CJK font; ASCII (English) -> the bold 20px montserrat so the
        // card title keeps its weight instead of shrinking to font_for's 14px.
        const lv_font_t *tf = ((unsigned char)cjk[0] >= 0x80)
            ? cjk_font() : &lv_font_montserrat_20;
        lv_obj_set_style_text_font(l, tf, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        if (prev) lv_obj_align_to(l, prev, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
        else lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, -2);
    }
}

static lv_obj_t *mk_body(lv_obj_t *card, const char *txt, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font_for(txt), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
    return l;
}

// --------------------------------------------------------- target form
// Overlay below the top bar: 名称/主机/端口/用户/密码/命令 textareas, an OS
// picker row of nerd-icon buttons, 保存/取消/删除 (inline confirm) and an
// lv_keyboard docked at the bottom for touch-only use.

#define FORM_H      (HOME_H - TOP_H)     // 648
#define KB_H        300                  // docked keyboard height
#define FIELD_W     460

static void focus_set(int idx)
{
    lv_obj_remove_state(s_ta[s_focus], LV_STATE_FOCUSED);
    s_focus = idx;
    lv_obj_add_state(s_ta[idx], LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_kb, s_ta[idx]);
    lv_obj_scroll_to_view(s_ta[idx], LV_ANIM_OFF);
}

static void mark_invalid(lv_obj_t *ta, bool bad)
{
    lv_obj_set_style_border_color(ta, lv_color_hex(bad ? 0xD64545 : 0xD8CFAE), 0);
    lv_obj_set_style_border_width(ta, bad ? 3 : 2, 0);
}

static void os_pick(int os)
{
    s_form_os = os;
    for (int i = 0; i < TARGET_OS_COUNT; i++) {
        lv_obj_set_style_border_width(s_os_btn[i], i == os ? 3 : 0, 0);
        lv_obj_set_style_bg_color(s_os_btn[i],
            lv_color_hex(i == os ? COL_NAVY : 0x8d8a7e), 0);
    }
}

static void os_btn_cb(lv_event_t *e)
{
    os_pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void auth_pick(int m)
{
    s_form_auth = m;
    for (int i = 0; i < TARGET_AUTH_COUNT; i++) {
        lv_obj_set_style_bg_color(s_auth_btn[i],
            lv_color_hex(i == m ? COL_NAVY : 0x8d8a7e), 0);
    }
}

static void auth_btn_cb(lv_event_t *e)
{
    auth_pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void del_reset(void)
{
    s_del_armed = false;
    if (s_del_lbl) {
        lv_label_set_text(s_del_lbl, T(T_DELETE));
        lv_obj_set_style_text_font(s_del_lbl, font_for(T(T_DELETE)), 0);
        lv_obj_set_style_bg_color(s_del_btn, lv_color_hex(0x9c3a28), 0);
    }
}

static void open_form(int idx)   // idx = target to edit, -1 = add new
{
    s_edit_idx = idx;
    const ssh_target_t *t = (idx >= 0) ? &s_cfg->targets[idx] : NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", t ? t->port : 22);
    lv_textarea_set_text(s_ta[TA_NAME], t ? t->name : "");
    lv_textarea_set_text(s_ta[TA_HOST], t ? t->host : "");
    lv_textarea_set_text(s_ta[TA_PORT], port);
    lv_textarea_set_text(s_ta[TA_USER], t ? t->user : "root");
    lv_textarea_set_text(s_ta[TA_PASS], t ? t->pass : "");
    lv_textarea_set_text(s_ta[TA_CMD],  t ? t->cmd  : "bash");
    for (int i = 0; i < TA_COUNT; i++) mark_invalid(s_ta[i], false);
    const char *ft = idx >= 0 ? T(T_EDIT_TARGET) : T(T_ADD_TARGET);
    lv_label_set_text(s_form_title, ft);
    lv_obj_set_style_text_font(s_form_title, font_for(ft), 0);
    os_pick(idx >= 0 && s_cfg->target_os[idx] < TARGET_OS_COUNT
            ? s_cfg->target_os[idx] : TARGET_OS_SERVER);
    auth_pick(idx >= 0 && s_cfg->target_auth[idx] < TARGET_AUTH_COUNT
            ? s_cfg->target_auth[idx] : TARGET_AUTH_AUTO);
    del_reset();
    if (idx >= 0) lv_obj_clear_flag(s_del_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_del_btn, LV_OBJ_FLAG_HIDDEN);
    // A physical keyboard (USB or the Tab5 I2C one) makes the on-screen
    // keyboard dead weight — hide it and let the form breathe. The ⌨ toggle
    // in the form can always summon it back (e.g. USB unplugged mid-edit).
    if (hid_keyboard_hw_present()) lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    s_app = APP_SSH;
    s_view = VIEW_FORM;
    apply_view();
    focus_set(t ? TA_NAME : TA_HOST);
}

static void close_form(void)     // back to the expanded SSH app
{
    s_view = VIEW_APP;
    s_app = APP_SSH;
    apply_view();
}

static void after_settings_change(void)
{
    settings_save(s_cfg);
    refresh_targets();      // grid rows + tiles
}

static void form_save_cb(lv_event_t *e)
{
    const char *host = lv_textarea_get_text(s_ta[TA_HOST]);
    const char *user = lv_textarea_get_text(s_ta[TA_USER]);
    const char *ps   = lv_textarea_get_text(s_ta[TA_PORT]);
    int port = ps[0] ? atoi(ps) : 22;    // empty -> default 22

    bool ok = true;
    mark_invalid(s_ta[TA_HOST], host[0] == '\0');
    mark_invalid(s_ta[TA_USER], user[0] == '\0');
    mark_invalid(s_ta[TA_PORT], port < 1 || port > 65535);
    if (host[0] == '\0' || user[0] == '\0' || port < 1 || port > 65535) ok = false;
    if (!ok) return;

    ssh_target_t *t;
    int idx;
    if (s_edit_idx >= 0) {
        idx = s_edit_idx;
        t = &s_cfg->targets[idx];
    } else {
        if (s_cfg->n_targets >= MAX_SSH_TARGETS) return;   // belt and braces
        idx = s_cfg->n_targets++;
        t = &s_cfg->targets[idx];
    }
    memset(t, 0, sizeof(*t));
    strlcpy(t->name, lv_textarea_get_text(s_ta[TA_NAME]), sizeof(t->name));
    strlcpy(t->host, host, sizeof(t->host));
    t->port = port;
    strlcpy(t->user, user, sizeof(t->user));
    strlcpy(t->pass, lv_textarea_get_text(s_ta[TA_PASS]), sizeof(t->pass));
    strlcpy(t->cmd,  lv_textarea_get_text(s_ta[TA_CMD]),  sizeof(t->cmd));
    s_cfg->target_os[idx] = (uint8_t)s_form_os;
    s_cfg->target_auth[idx] = (uint8_t)s_form_auth;
    // (a connected session to this target keeps its old creds until it
    // reconnects — ssh_client reads targets[] at connect time)
    after_settings_change();
    close_form();
}

static void form_cancel_cb(lv_event_t *e)  { close_form(); }

static void form_del_cb(lv_event_t *e)     // inline two-tap confirm
{
    if (!s_del_armed) {
        s_del_armed = true;
        lv_label_set_text(s_del_lbl, T(T_CONFIRM_DELETE));
        lv_obj_set_style_text_font(s_del_lbl, font_for(T(T_CONFIRM_DELETE)), 0);
        lv_obj_set_style_bg_color(s_del_btn, lv_color_hex(0x8E2418), 0);
        return;
    }
    int idx = s_edit_idx;
    if (idx < 0 || idx >= s_cfg->n_targets) { close_form(); return; }

    // A target with an open session: close that session first.
    for (int id = 0; id < MAX_SSH_SESSIONS; id++) {
        int tgt = -1;
        ssh_state(id, NULL, 0, &tgt);
        if (tgt == idx) ssh_close(id);
    }

    memmove(&s_cfg->targets[idx], &s_cfg->targets[idx + 1],
            (s_cfg->n_targets - idx - 1) * sizeof(ssh_target_t));
    memmove(&s_cfg->target_os[idx], &s_cfg->target_os[idx + 1],
            (s_cfg->n_targets - idx - 1) * sizeof(uint8_t));
    s_cfg->n_targets--;
    if (s_cfg->last_target >= s_cfg->n_targets) {
        s_cfg->last_target = s_cfg->n_targets ? s_cfg->n_targets - 1 : 0;
    }
    after_settings_change();
    close_form();
}

static void ta_focused_cb(lv_event_t *e)   // touch focus path
{
    s_focus = (int)(intptr_t)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(s_kb, s_ta[s_focus]);
}

static void kb_toggle_cb(lv_event_t *e)
{
    if (lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static void pass_eye_cb(lv_event_t *e)     // reveal toggle
{
    bool masked = lv_textarea_get_password_mode(s_ta[TA_PASS]);
    lv_textarea_set_password_mode(s_ta[TA_PASS], !masked);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0),
                      masked ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

// One labeled one-line textarea in a 2-column grid (reading order = tab
// order: 名称 主机 / 端口 用户 / 密码 命令).
static void mk_field(const char *label, int idx, int col, int row, int maxlen)
{
    int y = 70 + row * 62;
    lv_obj_t *l = lv_label_create(s_form);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, font_for(label), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_INK), 0);
    lv_obj_set_pos(l, col ? 652 : MARGIN_X, y + 8);

    lv_obj_t *ta = lv_textarea_create(s_form);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxlen);
    lv_obj_set_size(ta, FIELD_W, 46);
    lv_obj_set_pos(ta, col ? 744 : 120, y);
    lv_obj_set_style_radius(ta, 10, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0xD8CFAE), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_NAVY), LV_STATE_FOCUSED);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(ta, ta_focused_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)idx);
    s_ta[idx] = ta;
}

// Form footer button (保存/取消/删除/⌨), all on the OS-picker row.
static lv_obj_t *mk_form_btn(const char *txt, uint32_t bg, lv_event_cb_t cb,
                             int x, int w)
{
    lv_obj_t *btn = lv_button_create(s_form);
    lv_obj_set_size(btn, w, 52);
    lv_obj_set_pos(btn, x, 70 + 3 * 62 + 2);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font_for(txt), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    return btn;
}

static void mk_form(void)
{
    s_form = lv_obj_create(s_root);
    lv_obj_set_size(s_form, HOME_W, FORM_H);
    lv_obj_set_pos(s_form, 0, TOP_H);
    lv_obj_set_style_bg_color(s_form, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_form, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_form, 0, 0);
    lv_obj_set_style_radius(s_form, 0, 0);
    lv_obj_set_style_pad_all(s_form, 0, 0);
    lv_obj_clear_flag(s_form, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_form, LV_OBJ_FLAG_HIDDEN);

    s_form_title = lv_label_create(s_form);
    lv_label_set_text(s_form_title, T(T_EDIT_TARGET));
    lv_obj_set_style_text_font(s_form_title, font_for(T(T_EDIT_TARGET)), 0);
    lv_obj_set_style_text_color(s_form_title, lv_color_hex(COL_INK), 0);
    lv_obj_set_pos(s_form_title, MARGIN_X, 16);

    lv_obj_t *hint = lv_label_create(s_form);
    lv_label_set_text(hint, "Tab/Enter next field   Esc back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8d8a7e), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -MARGIN_X, 24);

    mk_field(T(T_NAME),     TA_NAME, 0, 0, sizeof(((ssh_target_t *)0)->name) - 1);
    mk_field(T(T_HOST),     TA_HOST, 1, 0, sizeof(((ssh_target_t *)0)->host) - 1);
    mk_field(T(T_PORT),     TA_PORT, 0, 1, 5);
    mk_field(T(T_USER),     TA_USER, 1, 1, sizeof(((ssh_target_t *)0)->user) - 1);
    mk_field(T(T_PASSWORD), TA_PASS, 0, 2, sizeof(((ssh_target_t *)0)->pass) - 1);
    mk_field(T(T_COMMAND),  TA_CMD,  1, 2, sizeof(((ssh_target_t *)0)->cmd) - 1);

    lv_textarea_set_password_mode(s_ta[TA_PASS], true);
    lv_obj_set_width(s_ta[TA_PASS], FIELD_W - 56);     // room for the eye
    lv_obj_t *eye = lv_button_create(s_form);
    lv_obj_set_size(eye, 46, 46);
    lv_obj_set_pos(eye, 120 + FIELD_W - 46, 70 + 2 * 62);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COL_NAVY), 0);
    lv_obj_set_style_radius(eye, 10, 0);
    lv_obj_set_style_shadow_width(eye, 0, 0);
    lv_obj_add_event_cb(eye, pass_eye_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(eye);
    lv_label_set_text(el, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(el, lv_color_hex(0xffffff), 0);
    lv_obj_center(el);

    // ----- OS picker row (nerd32 icon buttons), left side of the footer row
    int fy = 70 + 3 * 62 + 2;
    lv_obj_t *ol = lv_label_create(s_form);
    lv_label_set_text(ol, T(T_SYSTEM));
    lv_obj_set_style_text_font(ol, font_for(T(T_SYSTEM)), 0);
    lv_obj_set_style_text_color(ol, lv_color_hex(COL_INK), 0);
    lv_obj_set_pos(ol, MARGIN_X, fy + 12);
    for (int i = 0; i < TARGET_OS_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_form);
        lv_obj_set_size(btn, 52, 52);
        lv_obj_set_pos(btn, 120 + i * 60, fy);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x8d8a7e), 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_INK), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, os_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = mk_os_icon(btn, i, 32, &nerd32, 0xffffff);
        lv_obj_center(l);
        s_os_btn[i] = btn;
    }

    // ----- auth method row: 自动 / 密码 / 证书 (segmented buttons)
    int ay = fy + 64;
    lv_obj_t *al2 = lv_label_create(s_form);
    lv_label_set_text(al2, T(T_AUTH));
    lv_obj_set_style_text_font(al2, font_for(T(T_AUTH)), 0);
    lv_obj_set_style_text_color(al2, lv_color_hex(COL_INK), 0);
    lv_obj_set_pos(al2, MARGIN_X, ay + 12);
    const char *auth_names[TARGET_AUTH_COUNT] = { T(T_AUTH_AUTO), T(T_AUTH_PASSWORD), T(T_AUTH_CERT) };
    for (int i = 0; i < TARGET_AUTH_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_form);
        lv_obj_set_size(btn, 96, 48);
        lv_obj_set_pos(btn, 120 + i * 106, ay);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x8d8a7e), 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, auth_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, auth_names[i]);
        lv_obj_set_style_text_font(l, font_for(auth_names[i]), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_center(l);
        s_auth_btn[i] = btn;
    }

    // ----- footer buttons, right side of the same row
    mk_form_btn(T(T_SAVE), COL_GREEN, form_save_cb, 652, 130);
    mk_form_btn(T(T_CANCEL), 0x8d8a7e, form_cancel_cb, 652 + 142, 130);
    s_del_btn = mk_form_btn(T(T_DELETE), 0x9c3a28, form_del_cb, 652 + 284, 150);
    s_del_lbl = lv_obj_get_child(s_del_btn, 0);
    lv_obj_t *kbbtn = mk_form_btn(LV_SYMBOL_KEYBOARD, 0x6b6856, kb_toggle_cb,
                                  652 + 446, 64);
    // LV_SYMBOL_* glyphs live in the montserrat builtins, not the CJK font.
    lv_obj_set_style_text_font(lv_obj_get_child(kbbtn, 0),
                               &lv_font_montserrat_14, 0);

    s_kb = lv_keyboard_create(s_form);                 // qwerty, docked
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(s_kb, HOME_W, KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ------------------------------------------------------------- SSH key panel
// Covers the expanded SSH app content (same pattern as the target form):
// status line + scrollable read-only public-line textarea + 重新生成 / 删除
// (both two-tap inline confirm). Opened by the 密钥 chip on the SSH card.

static void open_keys(void)
{
    s_app = APP_SSH;
    s_view = VIEW_KEYS;
    s_key_regen_armed = s_key_del_armed = false;
    apply_view();
}

static void close_keys(void)
{
    s_view = VIEW_APP;
    s_app = APP_SSH;
    apply_view();
}

static void key_chip_cb(lv_event_t *e)   { open_keys(); }
static void keys_back_cb(lv_event_t *e)  { close_keys(); }

static void key_regen_reset(void)
{
    s_key_regen_armed = false;
    lv_label_set_text(s_key_regen_lbl, T(T_REGEN));
    lv_obj_set_style_text_font(s_key_regen_lbl, font_for(T(T_REGEN)), 0);
    lv_obj_set_style_bg_color(s_key_regen_btn, lv_color_hex(COL_NAVY), 0);
}

static void key_del_reset(void)
{
    s_key_del_armed = false;
    lv_label_set_text(s_key_del_lbl, T(T_DELETE));
    lv_obj_set_style_text_font(s_key_del_lbl, font_for(T(T_DELETE)), 0);
    lv_obj_set_style_bg_color(s_key_del_btn, lv_color_hex(0x9c3a28), 0);
}

static void key_regen_cb(lv_event_t *e)  // two-tap confirm -> wipe + keygen
{
    if (ssh_keys_status() == SSH_KEYS_GENERATING) return;
    if (!s_key_regen_armed) {
        s_key_regen_armed = true;
        key_del_reset();
        lv_label_set_text(s_key_regen_lbl, T(T_CONFIRM_REGEN));
        lv_obj_set_style_text_font(s_key_regen_lbl, font_for(T(T_CONFIRM_REGEN)), 0);
        lv_obj_set_style_bg_color(s_key_regen_btn, lv_color_hex(0x1f2a3a), 0);
        return;
    }
    ssh_keys_regen();         // wipes NVS+RAM, spawns internal-stack keygen task
    key_regen_reset();
    refresh_keys();           // flip status to 生成中… right away
}

static void key_del_cb(lv_event_t *e)    // two-tap confirm -> wipe, no regen
{
    if (ssh_keys_status() == SSH_KEYS_GENERATING) return;
    if (!s_key_del_armed) {
        s_key_del_armed = true;
        key_regen_reset();
        lv_label_set_text(s_key_del_lbl, T(T_CONFIRM_DELETE));
        lv_obj_set_style_text_font(s_key_del_lbl, font_for(T(T_CONFIRM_DELETE)), 0);
        lv_obj_set_style_bg_color(s_key_del_btn, lv_color_hex(0x8E2418), 0);
        return;
    }
    ssh_keys_delete();        // LVGL task has an internal stack: NVS write is fine
    key_del_reset();
    refresh_keys();
}

static lv_obj_t *mk_key_btn(const char *txt, uint32_t bg, lv_event_cb_t cb,
                            int x, int w, lv_obj_t **out_lbl)
{
    lv_obj_t *btn = lv_button_create(s_keys);
    lv_obj_set_size(btn, w, 52);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, -24);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font_for(txt), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return btn;
}

// The key panel sits exactly over the expanded SSH app content (not the dock).
static void mk_keys(void)
{
    s_keys = lv_obj_create(s_root);
    lv_obj_set_size(s_keys, APP_W, APP_H);
    lv_obj_set_pos(s_keys, MARGIN_X, APP_Y);
    lv_obj_set_style_bg_color(s_keys, lv_color_hex(COL_NAVY), 0);
    lv_obj_set_style_bg_opa(s_keys, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_keys, 24, 0);
    lv_obj_set_style_border_width(s_keys, 0, 0);
    lv_obj_set_style_pad_all(s_keys, 24, 0);
    lv_obj_clear_flag(s_keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_keys, LV_OBJ_FLAG_HIDDEN);

    // header: back button + title
    lv_obj_t *back = lv_button_create(s_keys);
    lv_obj_set_size(back, 56, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x46566e), 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, keys_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(s_keys);
    lv_label_set_text(title, T(T_DEVICE_KEY));
    lv_obj_set_style_text_font(title, font_for(T(T_DEVICE_KEY)), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 72, 10);

    // status line: 已就绪/生成中…/无密钥 + RSA-2048
    s_key_status_cjk = lv_label_create(s_keys);
    lv_label_set_text(s_key_status_cjk, T(T_NO_KEY));
    lv_obj_set_style_text_font(s_key_status_cjk, font_for(T(T_NO_KEY)), 0);
    lv_obj_set_style_text_color(s_key_status_cjk, lv_color_hex(0x7CFC9A), 0);
    lv_obj_align(s_key_status_cjk, LV_ALIGN_TOP_LEFT, 0, 64);
    s_key_status_lat = lv_label_create(s_keys);
    lv_label_set_text(s_key_status_lat, "");
    lv_obj_set_style_text_font(s_key_status_lat, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_key_status_lat, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(s_key_status_lat, s_key_status_cjk,
                    LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    // scrollable read-only public line
    s_key_pub_ta = lv_textarea_create(s_keys);
    lv_obj_set_size(s_key_pub_ta, APP_W - 48, 150);
    lv_obj_align(s_key_pub_ta, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_textarea_set_one_line(s_key_pub_ta, false);
    lv_textarea_set_text(s_key_pub_ta, "(no key)");
    lv_obj_add_state(s_key_pub_ta, LV_STATE_DISABLED);   // read-only
    lv_obj_set_style_bg_color(s_key_pub_ta, lv_color_hex(0x2a3445), 0);
    lv_obj_set_style_border_color(s_key_pub_ta, lv_color_hex(0x46566e), 0);
    lv_obj_set_style_border_width(s_key_pub_ta, 2, 0);
    lv_obj_set_style_radius(s_key_pub_ta, 12, 0);
    lv_obj_set_style_text_font(s_key_pub_ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_key_pub_ta, lv_color_hex(0xdce4f2), 0);
    lv_obj_set_style_text_color(s_key_pub_ta, lv_color_hex(0xdce4f2),
                               LV_PART_MAIN | LV_STATE_DISABLED);

    // Hints mix CJK + latin; neither font covers the other script, so build
    // each line as cjk-segment + montserrat-segment laid out side by side.
    lv_obj_t *h1a = lv_label_create(s_keys);
    lv_label_set_text(h1a, T(T_PUBKEY_HINT_AT));
    lv_obj_set_style_text_font(h1a, font_for(T(T_PUBKEY_HINT_AT)), 0);
    lv_obj_set_style_text_color(h1a, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align(h1a, LV_ALIGN_TOP_LEFT, 0, 264);
    lv_obj_t *h1b = lv_label_create(s_keys);
    lv_label_set_text(h1b, "t5.cc.hn");
    lv_obj_set_style_text_font(h1b, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h1b, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(h1b, h1a, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *h1c = lv_label_create(s_keys);
    lv_label_set_text(h1c, T(T_PUBKEY_HINT_COPY));
    lv_obj_set_style_text_font(h1c, font_for(T(T_PUBKEY_HINT_COPY)), 0);
    lv_obj_set_style_text_color(h1c, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(h1c, h1b, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *h1d = lv_label_create(s_keys);
    lv_label_set_text(h1d, "shell");
    lv_obj_set_style_text_font(h1d, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h1d, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(h1d, h1c, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *h1e = lv_label_create(s_keys);
    lv_label_set_text(h1e, T(T_PUBKEY_HINT_RUN));
    lv_obj_set_style_text_font(h1e, font_for(T(T_PUBKEY_HINT_RUN)), 0);
    lv_obj_set_style_text_color(h1e, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(h1e, h1d, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *h1f = lv_label_create(s_keys);
    lv_label_set_text(h1f, "sshkey");
    lv_obj_set_style_text_font(h1f, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h1f, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align_to(h1f, h1e, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_obj_t *h2a = lv_label_create(s_keys);
    lv_label_set_text(h2a, T(T_PRIVKEY_HINT));
    lv_obj_set_style_text_font(h2a, font_for(T(T_PRIVKEY_HINT)), 0);
    lv_obj_set_style_text_color(h2a, lv_color_hex(0x9aa3b5), 0);
    lv_obj_align(h2a, LV_ALIGN_TOP_LEFT, 0, 300);
    lv_obj_t *h2b = lv_label_create(s_keys);
    lv_label_set_text(h2b, "(t5.cc.hn)");
    lv_obj_set_style_text_font(h2b, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h2b, lv_color_hex(0x9aa3b5), 0);
    lv_obj_align_to(h2b, h2a, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    s_key_regen_btn = mk_key_btn(T(T_REGEN), COL_NAVY, key_regen_cb,
                                 0, 180, &s_key_regen_lbl);
    s_key_del_btn   = mk_key_btn(T(T_DELETE), 0x9c3a28, key_del_cb,
                                 196, 150, &s_key_del_lbl);
}

// UI-DEV HELPER for GET /shot?...&keys=1: open the SSH key panel.
void ui_home_set_keys_view(int on)
{
    if (!s_root) return;
    ui_home_set_open(true);
    bsp_display_lock(0);
    if (on) {
        open_keys();
    } else {
        s_view = VIEW_APP;
        s_app = APP_SSH;
        apply_view();
    }
    bsp_display_unlock();
}

// UI-DEV HELPER for GET /shot?...&conn=N: 0 grid, 1 expanded 连接 app,
// 2 添加网络 form.
void ui_home_set_conn_view(int mode)
{
    if (!s_root) return;
    ui_home_set_open(true);
    bsp_display_lock(0);
    if (mode == 2) {
        s_app = APP_CONN;
        s_view = VIEW_APP;
        apply_view();
        wifiadd_open();
    } else if (mode == 1) {
        s_app = APP_CONN;
        s_view = VIEW_APP;
        apply_view();
    } else {
        s_view = VIEW_GRID;
        apply_view();
    }
    bsp_display_unlock();
}

// UI-DEV HELPER for GET /shot?...&lang=0|1: force UI language + live rebuild.
// Snapshot-only; does not touch settings.lang.
void ui_home_set_lang_dev(int lang)
{
    if (!s_root) return;
    if (lang < 0 || lang >= LANG_COUNT) return;
    if ((lang_t)lang == i18n_lang()) return;
    i18n_set_lang((lang_t)lang);
    ui_home_rebuild();
}

// ------------------------------------------------------------- public API

bool ui_home_handle_key(uint8_t usage, uint8_t modifiers)
{
    if (!ui_home_is_open()) return false;
    if (s_view == VIEW_GRID) return false;     // Esc there closes the panel

    bsp_display_lock(0);
    if (s_view == VIEW_FORM) {
        if (usage == HID_KEY_ESC) {
            close_form();
        } else if (usage == HID_KEY_DEL) {                 // backspace
            lv_textarea_delete_char(s_ta[s_focus]);
        } else if (usage == HID_KEY_TAB || usage == HID_KEY_ENTER) {
            focus_set((s_focus + 1) % TA_COUNT);
        } else {
            char ch = hid_usage_to_ascii(usage, modifiers);
            if (ch) lv_textarea_add_char(s_ta[s_focus], ch);
        }
    } else if (s_view == VIEW_WIFIADD) {
        lv_obj_t *ta = s_wifiadd_focus_ta ? s_wifiadd_focus_ta : s_wifiadd_ssid;
        if (usage == HID_KEY_ESC) {
            wifiadd_close();
        } else if (usage == HID_KEY_DEL) {
            lv_textarea_delete_char(ta);
        } else if (usage == HID_KEY_TAB) {
            ta = (ta == s_wifiadd_ssid) ? s_wifiadd_pass : s_wifiadd_ssid;
            s_wifiadd_focus_ta = ta;
            if (s_wifiadd_kb) lv_keyboard_set_textarea(s_wifiadd_kb, ta);
        } else if (usage == HID_KEY_ENTER) {
            wifiadd_save_cb(NULL);
        } else {
            char ch = hid_usage_to_ascii(usage, modifiers);
            if (ch) lv_textarea_add_char(ta, ch);
        }
    } else if (s_view == VIEW_KEYS) {
        if (usage == HID_KEY_ESC) close_keys();
    } else {                                   // VIEW_APP
        if (usage == HID_KEY_ESC) {
            s_view = VIEW_GRID;
            apply_view();
        }
        // other keys are swallowed while an app is expanded
    }
    bsp_display_unlock();
    return true;
}

// UI-DEV HELPER for GET /shot?...&edit=N (see web_config.c):
// 0 = grid, 1 = expanded SSH app (tiles + dock), 2 = blank add form.
void ui_home_set_edit_view(int mode)
{
    if (!s_root) return;
    ui_home_set_open(true);
    bsp_display_lock(0);
    if (mode == 2) {
        open_form(-1);
    } else if (mode == 1) {
        s_view = VIEW_APP;
        s_app = APP_SSH;
        apply_view();
    } else {
        s_view = VIEW_GRID;
        apply_view();
    }
    bsp_display_unlock();
}

// Targets changed elsewhere (web config POST, ssh open/switch). Safe from
// any task: takes the LVGL lock; cheap no-op while the panel is closed.
void ui_home_refresh(void)
{
    if (!s_root || !s_open) return;
    bsp_display_lock(0);
    refresh_targets();
    bsp_display_unlock();
}

// 连接 card content: a sparse grid face (wifi/bt icon + name per half) plus a
// full-detail container shown only while the app is expanded.
static void mk_conn_card(lv_obj_t *card)
{
    // ---- sparse grid face: two halves, icon + name only ----
    s_conn_face = lv_obj_create(card);
    lv_obj_set_size(s_conn_face, LV_PCT(100), CARD_H - 80);
    lv_obj_align(s_conn_face, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_obj_set_style_bg_opa(s_conn_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_conn_face, 0, 0);
    lv_obj_set_style_pad_all(s_conn_face, 0, 0);
    lv_obj_clear_flag(s_conn_face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Two equal halves, each a centered icon-over-name stack. A faint vertical
    // hairline divides Wi-Fi from 蓝牙. Icon and name share the same center x,
    // so they read as one aligned unit.
    int half_w = (CARD_W - 40) / 2;
    int face_h = CARD_H - 80;

    lv_obj_t *divider = lv_obj_create(s_conn_face);
    lv_obj_set_size(divider, 1, face_h - 24);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    // left half: wifi
    lv_obj_t *wicon = lv_label_create(s_conn_face);
    lv_label_set_text(wicon, "\xEF\x87\xAB");           // U+F1EB wifi
    lv_obj_set_style_text_font(wicon, &nerd64, 0);
    lv_obj_set_style_text_color(wicon, lv_color_hex(COL_INK2), 0);
    lv_obj_align(wicon, LV_ALIGN_TOP_MID, -half_w / 2, 14);
    s_wifi_face_name = lv_label_create(s_conn_face);
    lv_label_set_text(s_wifi_face_name, "Wi-Fi");
    lv_obj_set_style_text_font(s_wifi_face_name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi_face_name, lv_color_hex(COL_INK2), 0);
    lv_label_set_long_mode(s_wifi_face_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_wifi_face_name, half_w - 16);
    lv_obj_set_style_text_align(s_wifi_face_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_face_name, LV_ALIGN_TOP_MID, -half_w / 2, 92);

    // right half: bluetooth
    lv_obj_t *bicon = lv_label_create(s_conn_face);
    lv_label_set_text(bicon, "\xEF\x8A\x93");           // U+F293 bluetooth
    lv_obj_set_style_text_font(bicon, &nerd64, 0);
    lv_obj_set_style_text_color(bicon, lv_color_hex(COL_INK2), 0);
    lv_obj_align(bicon, LV_ALIGN_TOP_MID, half_w / 2, 14);
    lv_obj_t *bname = lv_label_create(s_conn_face);
    lv_label_set_text(bname, T(T_BT));
    lv_obj_set_style_text_font(bname, font_for(T(T_BT)), 0);
    lv_obj_set_style_text_color(bname, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_text_align(bname, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bname, LV_ALIGN_TOP_MID, half_w / 2, 92);

    // ---- expanded detail container (hidden until APP_CONN expands) ----
    s_conn_detail = lv_obj_create(card);
    lv_obj_set_size(s_conn_detail, LV_PCT(100), APP_H - 80);
    lv_obj_align(s_conn_detail, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_opa(s_conn_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_conn_detail, 0, 0);
    lv_obj_set_style_pad_all(s_conn_detail, 0, 0);
    lv_obj_clear_flag(s_conn_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_conn_detail, LV_OBJ_FLAG_HIDDEN);

    int colw = (APP_W - 40) / 2 - 16;   // two columns inside the card

    // vertical hairline between the Wi-Fi and 蓝牙 columns
    lv_obj_t *vdiv = lv_obj_create(s_conn_detail);
    lv_obj_set_size(vdiv, 1, APP_H - 80 - 24);
    lv_obj_set_pos(vdiv, colw + 15, 6);
    lv_obj_set_style_bg_color(vdiv, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_bg_opa(vdiv, LV_OPA_20, 0);
    lv_obj_set_style_border_width(vdiv, 0, 0);

    // --- Wi-Fi section (left column) ---
    lv_obj_t *wt = lv_label_create(s_conn_detail);
    lv_label_set_text(wt, "Wi-Fi");
    lv_obj_set_style_text_font(wt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(wt, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(wt, 0, 0);

    s_wifi_cur_label = lv_label_create(s_conn_detail);
    lv_label_set_text(s_wifi_cur_label, "...");
    lv_obj_set_style_text_font(s_wifi_cur_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_cur_label, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_text_line_space(s_wifi_cur_label, 6, 0);
    lv_obj_set_pos(s_wifi_cur_label, 0, 44);

    lv_obj_t *sl = lv_label_create(s_conn_detail);
    lv_label_set_text(sl, T(T_SAVED_NETS));
    lv_obj_set_style_text_font(sl, font_for(T(T_SAVED_NETS)), 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(sl, 0, 96);

    s_wifi_list = lv_obj_create(s_conn_detail);
    lv_obj_set_size(s_wifi_list, colw, APP_H - 80 - 200);
    lv_obj_set_pos(s_wifi_list, 0, 128);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 8, 0);

    lv_obj_t *addb = lv_button_create(s_conn_detail);
    lv_obj_set_size(addb, 160, 48);
    lv_obj_set_pos(addb, 0, APP_H - 80 - 60);
    lv_obj_set_style_bg_color(addb, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_radius(addb, 16, 0);
    lv_obj_set_style_shadow_width(addb, 0, 0);
    lv_obj_add_event_cb(addb, wifi_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *addl = lv_label_create(addb);
    lv_label_set_text(addl, T(T_ADD_NET));
    lv_obj_set_style_text_font(addl, font_for(T(T_ADD_NET)), 0);
    lv_obj_set_style_text_color(addl, lv_color_hex(0xffffff), 0);
    lv_obj_center(addl);

    // --- 蓝牙 section (right column) ---
    int bx = colw + 32;
    lv_obj_t *bt = lv_label_create(s_conn_detail);
    lv_label_set_text(bt, T(T_BT));
    lv_obj_set_style_text_font(bt, font_for(T(T_BT)), 0);
    lv_obj_set_style_text_color(bt, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(bt, bx, 0);

    s_ble_state_label = lv_label_create(s_conn_detail);
    lv_label_set_text(s_ble_state_label, "...");
    lv_obj_set_style_text_font(s_ble_state_label, cjk_font(), 0);
    lv_obj_set_style_text_color(s_ble_state_label, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(s_ble_state_label, bx, 48);

    s_ble_btn = lv_button_create(s_conn_detail);     // adv Start/Stop
    lv_obj_set_size(s_ble_btn, 160, 48);
    lv_obj_set_pos(s_ble_btn, bx, 92);
    lv_obj_set_style_bg_color(s_ble_btn, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_radius(s_ble_btn, 16, 0);
    lv_obj_set_style_shadow_width(s_ble_btn, 0, 0);
    lv_obj_add_event_cb(s_ble_btn, ble_btn_cb, LV_EVENT_CLICKED, NULL);
    s_ble_btn_label = lv_label_create(s_ble_btn);
    lv_label_set_text(s_ble_btn_label, T(T_START_ADV));
    lv_obj_set_style_text_font(s_ble_btn_label, font_for(T(T_START_ADV)), 0);
    lv_obj_set_style_text_color(s_ble_btn_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_ble_btn_label);

    // BLE master enable: a labeled switch (蓝牙 [====O]) below the adv button.
    s_ble_master_label = lv_label_create(s_conn_detail);
    lv_label_set_text(s_ble_master_label, T(T_BT_SWITCH));
    lv_obj_set_style_text_font(s_ble_master_label, font_for(T(T_BT_SWITCH)), 0);
    lv_obj_set_style_text_color(s_ble_master_label, lv_color_hex(COL_INK2), 0);
    lv_obj_set_pos(s_ble_master_label, bx, 160);

    s_ble_master_btn = lv_switch_create(s_conn_detail);
    lv_obj_set_size(s_ble_master_btn, 64, 34);
    lv_obj_set_pos(s_ble_master_btn, bx + 96, 156);
    lv_obj_set_style_bg_color(s_ble_master_btn, lv_color_hex(COL_INK2),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_ble_master_btn, ble_master_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Builds the whole panel tree under a fresh s_root. LVGL lock must be held.
// Called by ui_home_create() (first build) and ui_home_rebuild() (language
// switch). Resets s_wifiadd to NULL so the mini-form is lazily rebuilt.
static void build_home(void)
{
    s_wifiadd = NULL;     // child of the old s_root, already deleted

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_root, HOME_W, HOME_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    // ----- top bar: wordmark + hint left, battery + clock right
    lv_obj_t *mark = lv_label_create(s_root);
    lv_label_set_text(mark, "tab5");
    lv_obj_set_style_text_font(mark, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(mark, lv_color_hex(COL_INK), 0);
    lv_obj_set_pos(mark, MARGIN_X, 20);

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "Ctrl+Alt+P / Esc: terminal");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8d8a7e), 0);
    lv_obj_align_to(hint, mark, LV_ALIGN_OUT_RIGHT_BOTTOM, 16, -4);

    s_clock_label = lv_label_create(s_root);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(COL_INK), 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -MARGIN_X, 20);

    s_batt_label = lv_label_create(s_root);
    lv_label_set_text(s_batt_label, "");
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(0x6d6a5e), 0);
    lv_obj_align_to(s_batt_label, s_clock_label, LV_ALIGN_OUT_LEFT_MID, -16, 0);

    // ----- Wi-Fi cluster: nerd wifi glyph + IP/status, left of the battery.
    // Color tells state (grey off / amber connecting / RSSI-graded green->red
    // when connected); the text shows the IP when connected.
    s_wifi_top_text = lv_label_create(s_root);
    lv_label_set_text(s_wifi_top_text, "");
    lv_obj_set_style_text_font(s_wifi_top_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_top_text, lv_color_hex(0x6d6a5e), 0);

    s_wifi_top_icon = lv_label_create(s_root);
    lv_label_set_text(s_wifi_top_icon, "\xEF\x87\xAB");   // U+F1EB wifi
    lv_obj_set_style_text_font(s_wifi_top_icon, &nerd32, 0);
    lv_obj_set_style_text_color(s_wifi_top_icon, lv_color_hex(0x9aa3a8), 0);
    lv_obj_align(s_wifi_top_icon, LV_ALIGN_TOP_RIGHT, -260, 22);
    lv_obj_align_to(s_wifi_top_text, s_wifi_top_icon, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    // ----- card 1: SSH 会话 (navy)
    lv_obj_t *card = mk_card(APP_SSH);
    mk_title(card, "SSH", T(T_SSH_SESSIONS), 0xffffff);

    s_ssh_sub = lv_label_create(card);
    lv_label_set_text(s_ssh_sub, "0/0");
    lv_obj_set_style_text_font(s_ssh_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ssh_sub, lv_color_hex(0xb8c2d6), 0);
    lv_obj_align(s_ssh_sub, LV_ALIGN_TOP_RIGHT, 0, 4);

    // "blank area = management" affordance: a ghost chip in the corner.
    // It's passive — the tap bubbles to the card and expands the app, same
    // as tapping any other empty spot; the chip just names that behavior.
    s_manage = lv_obj_create(card);
    lv_obj_t *manage = s_manage;
    lv_obj_set_size(manage, 96, 32);
    lv_obj_align(manage, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(manage, lv_color_hex(0x46566e), 0);
    lv_obj_set_style_bg_opa(manage, LV_OPA_60, 0);
    lv_obj_set_style_radius(manage, 16, 0);
    lv_obj_set_style_border_width(manage, 0, 0);
    lv_obj_set_style_pad_all(manage, 0, 0);
    lv_obj_clear_flag(manage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(manage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ml = lv_label_create(manage);
    lv_label_set_text(ml, T(T_MANAGE));
    lv_obj_set_style_text_font(ml, font_for(T(T_MANAGE)), 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(0xdce4f2), 0);
    lv_obj_align(ml, LV_ALIGN_CENTER, -8, 0);
    lv_obj_t *mr = lv_label_create(manage);
    lv_label_set_text(mr, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(mr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mr, lv_color_hex(0xdce4f2), 0);
    lv_obj_align(mr, LV_ALIGN_CENTER, 28, 0);

    s_ssh_list = lv_obj_create(card);
    lv_obj_set_size(s_ssh_list, LV_PCT(100), CARD_H - 40 - 36 - 36);
    lv_obj_align(s_ssh_list, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_obj_set_style_bg_opa(s_ssh_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ssh_list, 0, 0);
    lv_obj_set_style_pad_all(s_ssh_list, 0, 0);
    lv_obj_set_flex_flow(s_ssh_list, LV_FLEX_FLOW_ROW_WRAP);   // 3x2 icon cells
    lv_obj_set_style_pad_row(s_ssh_list, 8, 0);
    lv_obj_set_style_pad_column(s_ssh_list, 8, 0);
    // preview only — let taps fall through to the card's expand handler
    lv_obj_clear_flag(s_ssh_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ssh_list, LV_OBJ_FLAG_SCROLLABLE);

    // expanded-mode device tile grid (hidden while in the card grid)
    s_tiles = lv_obj_create(card);
    lv_obj_set_size(s_tiles, LV_PCT(100), APP_H - 40 - 60);
    lv_obj_align(s_tiles, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_opa(s_tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tiles, 0, 0);
    lv_obj_set_style_pad_all(s_tiles, 0, 0);
    lv_obj_set_flex_flow(s_tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_tiles, 16, 0);
    lv_obj_set_style_pad_row(s_tiles, 16, 0);
    lv_obj_add_flag(s_tiles, LV_OBJ_FLAG_HIDDEN);

    // ----- SSH card: 密钥 chip, top-right (only shown while SSH is expanded)
    s_key_chip = lv_button_create(card);
    lv_obj_set_size(s_key_chip, 96, 40);
    lv_obj_align(s_key_chip, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(s_key_chip, lv_color_hex(0x46566e), 0);
    lv_obj_set_style_radius(s_key_chip, 14, 0);
    lv_obj_set_style_shadow_width(s_key_chip, 0, 0);
    lv_obj_add_event_cb(s_key_chip, key_chip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kcl = lv_label_create(s_key_chip);
    lv_label_set_text(kcl, T(T_KEY));
    lv_obj_set_style_text_font(kcl, font_for(T(T_KEY)), 0);
    lv_obj_set_style_text_color(kcl, lv_color_hex(0xdce4f2), 0);
    lv_obj_center(kcl);
    lv_obj_add_flag(s_key_chip, LV_OBJ_FLAG_HIDDEN);

    // ----- card 2: 连接 (teal) — minimal grid face + rich expanded detail.
    card = mk_card(APP_CONN);
    mk_title(card, NULL, T(T_CONNECT), 0x0c3b40);
    mk_conn_card(card);

    // ----- card 4: 显示 (yellow)
    card = mk_card(APP_DISP);
    mk_title(card, NULL, T(T_DISPLAY), 0x4a3500);
    mk_body(card, "brightness", 0x4a3500, 50);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_size(slider, LV_PCT(100), 16);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 0, 84);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xE0A92C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4a3500), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x4a3500), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright_value = mk_body(card, "100%", 0x4a3500, 116);

    char sleep_txt[40];
    if (s_cfg->sleep_timeout_s) {
        snprintf(sleep_txt, sizeof(sleep_txt), "sleep after %u s idle",
                 (unsigned)s_cfg->sleep_timeout_s);
    } else {
        strcpy(sleep_txt, "sleep: never");
    }
    lv_obj_t *sl = mk_body(card, sleep_txt, 0x7a5a10, 0);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // ----- 语言 / Language toggle (segmented 中文 / English, auth-picker style).
    // Lives on the Display card so it appears in the expanded 显示 app. Tapping
    // an option switches language, persists settings.lang and live-rebuilds the
    // panel so every label updates immediately.
    lv_obj_t *ll = lv_label_create(card);
    lv_label_set_text(ll, T(T_LANGUAGE));
    lv_obj_set_style_text_font(ll, font_for(T(T_LANGUAGE)), 0);
    lv_obj_set_style_text_color(ll, lv_color_hex(0x4a3500), 0);
    lv_obj_set_pos(ll, 0, 150);
    static const str_id_t lang_label_id[LANG_COUNT] = { T_LANG_ZH, T_LANG_EN };
    for (int i = 0; i < LANG_COUNT; i++) {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, 120, 44);
        lv_obj_set_pos(b, i * 132, 182);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, lang_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, T(lang_label_id[i]));
        lv_obj_set_style_text_font(l, font_for(T(lang_label_id[i])), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
        lv_obj_center(l);
        s_lang_btn[i] = b;
    }
    lang_paint();

    // ----- card 5: 系统 (coral)
    card = mk_card(APP_SYS);
    mk_title(card, NULL, T(T_SYSTEM), 0xffffff);
    s_sys_label = mk_body(card, "...", 0xffffff, 44);
    lv_obj_set_style_text_line_space(s_sys_label, 8, 0);

    // Device PIN (gates web + BLE) + two-tap regenerate button.
    char pbuf[24];
    snprintf(pbuf, sizeof(pbuf), "PIN: %s", security_pin());
    s_pin_label = mk_body(card, pbuf, 0xffe9c2, 150);
    lv_obj_t *rb = lv_button_create(card);
    lv_obj_set_size(rb, 180, 44);
    lv_obj_set_pos(rb, 0, 182);
    lv_obj_set_style_radius(rb, 14, 0);
    lv_obj_set_style_shadow_width(rb, 0, 0);
    lv_obj_set_style_bg_color(rb, lv_color_hex(COL_NAVY), 0);
    lv_obj_add_event_cb(rb, pin_regen_cb, LV_EVENT_CLICKED, NULL);
    s_pin_regen_lbl = lv_label_create(rb);
    lv_label_set_text(s_pin_regen_lbl, T(T_REGEN_PIN));
    lv_obj_set_style_text_font(s_pin_regen_lbl, font_for(T(T_REGEN_PIN)), 0);
    lv_obj_set_style_text_color(s_pin_regen_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_center(s_pin_regen_lbl);
    s_pin_regen_armed = false;

    // ----- 穿透/Tunnel: enable switch + assigned-URL status line.
    // Reverse tunnel exposes this device's port 80 at a public subdomain.
    lv_obj_t *natl = lv_label_create(card);
    lv_label_set_text(natl, T(T_TUNNEL));
    lv_obj_set_style_text_font(natl, font_for(T(T_TUNNEL)), 0);
    lv_obj_set_style_text_color(natl, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(natl, 0, 244);

    s_nat_sw = lv_switch_create(card);
    lv_obj_set_size(s_nat_sw, 72, 38);
    lv_obj_set_pos(s_nat_sw, 108, 240);
    lv_obj_set_style_bg_color(s_nat_sw, lv_color_hex(COL_NAVY), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_cfg->nat_enabled) lv_obj_add_state(s_nat_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_nat_sw, nat_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Status line: assigned public host (ASCII) / 连接中 / 未启用.
    s_nat_status = lv_label_create(card);
    lv_label_set_text(s_nat_status, T(T_TUNNEL_OFF));
    lv_obj_set_style_text_font(s_nat_status, font_for(T(T_TUNNEL_OFF)), 0);
    lv_obj_set_style_text_color(s_nat_status, lv_color_hex(0xffe9c2), 0);
    lv_label_set_long_mode(s_nat_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_nat_status, CARD_W - 40);
    lv_obj_set_pos(s_nat_status, 0, 286);

    // ----- card 6: 输入法 (green)
    card = mk_card(APP_IME);
    mk_title(card, NULL, T(T_IME), 0xffffff);
    size_t dict_len = 0;
    const uint8_t *dict = assets_pinyin_dict(&dict_len);
    char ime_txt[64];
    if (dict) {
        snprintf(ime_txt, sizeof(ime_txt), "pinyin dict loaded (%u KB)",
                 (unsigned)(dict_len / 1024));
    } else {
        strcpy(ime_txt, "no dict on SD - IME unavailable");
    }
    mk_body(card, ime_txt, 0xffffff, 44);
    mk_body(card, dict ? "Ctrl+Space toggles pinyin" : "put dict_pinyin.dat in /sdcard/tab5",
            0xd8f2e2, 76);

    mk_dock();      // hidden icon dock (visible while an app is expanded)
    mk_form();      // hidden target form (OS picker + keyboard)
    mk_keys();      // hidden SSH key panel (overlays the expanded SSH app)

    s_timer = lv_timer_create(tick_cb, 1000, NULL);
    lv_timer_pause(s_timer);
}

void ui_home_create(settings_t *s)
{
    s_cfg = s;
    bsp_display_lock(0);
    build_home();
    bsp_display_unlock();
}

// Live language switch: tear down and recreate the panel so every label (incl.
// static ones) reflects the new language and font, then restore the open/view
// state. Cheaper than threading T() through hundreds of label handles, and the
// tree is small. LVGL lock taken internally.
static void ui_home_rebuild(void)
{
    bool was_open = s_open;
    home_view_t view = s_view;
    int app = s_app;

    bsp_display_lock(0);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    // These LVGL timers are NOT children of s_root: delete them explicitly.
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_wifi_pulse_timer) { lv_timer_delete(s_wifi_pulse_timer); s_wifi_pulse_timer = NULL; }
    if (s_root)  { lv_obj_delete(s_root); s_root = NULL; }
    // Overlays/toasts/sheets were children of s_root and are gone with it.
    s_toast = NULL; s_sheet = NULL;

    build_home();

    // Restore view/app, then re-open if it was open (apply_view + foreground).
    s_view = view;
    s_app  = app;
    s_open = false;             // force ui_home_set_open to act
    bsp_display_unlock();
    if (was_open) {
        ui_home_set_open(true);
    }
}

// ---------------------------------------------------------------- toggling

bool ui_home_is_open(void)
{
    return s_open;
}

void ui_home_set_open(bool open)
{
    if (!s_root || open == s_open) return;
    bsp_display_lock(0);
    if (open) {
        refresh_all();
        apply_view();
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
        lv_timer_resume(s_timer);
    } else {
        s_view = VIEW_GRID;             // closing forgets launcher state
        apply_view();
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(s_timer);
    }
    s_open = open;
    bsp_display_unlock();
}

bool ui_home_toggle(void)
{
    ui_home_set_open(!s_open);
    return s_open;
}

// UI-DEV helper for /shot snapshots.
void ui_home_show_sheet(int target_idx)
{
    bsp_display_lock(0);
    sheet_show(target_idx);
    bsp_display_unlock();
}
