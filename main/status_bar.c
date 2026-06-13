// LVGL status bar across the top of the screen (TAB5_STATUS_BAR_H tall).
// Layout: [● ssh-target] [IME] ... HH:MM ... [SSID RSSI] [IP] [temp] [batt].
// One 1s lv_timer updates clock+temp every tick and the slower sources
// (Wi-Fi, battery, SSH state) every 5th tick. All reads are non-blocking:
// power_mon caches from its own task, the temp sensor read is fast, and
// esp_wifi_sta_get_ap_info is cheap at 5s.

#include "status_bar.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/temperature_sensor.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "term.h"
#include "wifi.h"
#include "assets.h"
#include "ssh_client.h"
#include "power_mon.h"

static const char *TAG = "status_bar";

#define BAR_W 1280

static lv_obj_t *s_bar;
static lv_obj_t *s_ssh_dot;
static lv_obj_t *s_ssh_label;
static lv_obj_t *s_ime_label;
static lv_obj_t *s_rec_label;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_right_label;     // "SSID -55dBm  10.0.0.5  43°C  4.05V 87%"

static temperature_sensor_handle_t s_temp;
static bool s_temp_ok;

static void temp_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_temp) == ESP_OK
        && temperature_sensor_enable(s_temp) == ESP_OK) {
        s_temp_ok = true;
    } else {
        ESP_LOGW(TAG, "internal temp sensor unavailable");
    }
}

// Console `temp` command: the bar owns the sensor handle.
bool status_bar_get_temp(float *celsius)
{
    if (!s_temp_ok) return false;
    return temperature_sensor_get_celsius(s_temp, celsius) == ESP_OK;
}

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char buf[8];
    if (tm_local.tm_year + 1900 < 2025) {
        strcpy(buf, "--:--");       // clock not set yet
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    }
    lv_label_set_text(s_clock_label, buf);
}

static void update_ssh(void)
{
    char target[48];
    ssh_state_t st = ssh_client_get_state(target, sizeof(target));
    uint32_t color;
    switch (st) {
    case SSH_STATE_CONNECTED:  color = 0x40d060; break;
    case SSH_STATE_CONNECTING: color = 0xe0b040; break;
    default:                   color = 0x707070; break;
    }
    lv_obj_set_style_text_color(s_ssh_dot, lv_color_hex(color), 0);

    // With several sessions open, prefix the active one's position: "2/3 mac".
    int count = ssh_session_count(), active = ssh_active();
    if (count > 1 && active >= 0) {
        int pos = 0;
        for (int id = 0; id <= active; id++) {
            int tgt;
            ssh_state(id, NULL, 0, &tgt);
            if (tgt >= 0) pos++;            // open slot (target_idx valid)
        }
        char buf[72];
        snprintf(buf, sizeof(buf), "%d/%d %s", pos, count, target);
        lv_label_set_text(s_ssh_label, buf);
    } else if (count > 0 && target[0]) {
        lv_label_set_text(s_ssh_label, target);
    } else {
        lv_label_set_text(s_ssh_label, "no ssh");
    }
    lv_obj_set_style_text_color(s_ssh_label,
                                lv_color_hex(count > 0 ? 0xd0d0d0 : 0x707070), 0);
}

static void update_right(void)
{
    char buf[128];
    int len = 0;

    wifi_ap_record_t ap;
    char ip[20];
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && wifi_get_ip(ip, sizeof(ip))) {
        len += snprintf(buf + len, sizeof(buf) - len, "%s %ddBm  %s",
                        (const char *)ap.ssid, ap.rssi, ip);
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, "no wifi");
    }

    if (s_temp_ok) {
        float c;
        if (temperature_sensor_get_celsius(s_temp, &c) == ESP_OK) {
            len += snprintf(buf + len, sizeof(buf) - len, "  %.0f°C", c);
        }
    }

    float volts;
    int pct;
    bool chg;
    if (power_mon_get(&volts, &pct, &chg)) {
        len += snprintf(buf + len, sizeof(buf) - len, "  %.2fV %d%%%s",
                        volts, pct, chg ? " " LV_SYMBOL_CHARGE : "");
    }

    lv_label_set_text(s_right_label, buf);
}

// Runs in the LVGL task: no bsp_display_lock needed.
static void tick_cb(lv_timer_t *timer)
{
    static int div;
    update_clock();
    if (div++ % 5 == 0) {
        update_ssh();
        update_right();
    }
}

static lv_obj_t *mk_label(const char *txt, uint32_t color)
{
    lv_obj_t *l = lv_label_create(s_bar);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void status_bar_init(void)
{
    temp_init();

    bsp_display_lock(0);

    s_bar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_bar, BAR_W, TAB5_STATUS_BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_bar, lv_color_hex(0x383838), 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_bar, 10, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    // left: SSH dot + target, then IME mode
    s_ssh_dot = mk_label(LV_SYMBOL_STOP, 0x707070);   // small square as the dot
    lv_obj_align(s_ssh_dot, LV_ALIGN_LEFT_MID, 0, 0);

    s_ssh_label = mk_label("no target", 0xd0d0d0);
    lv_obj_align_to(s_ssh_label, s_ssh_dot, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    s_ime_label = mk_label("EN", 0x80a0e0);
    lv_obj_align(s_ime_label, LV_ALIGN_LEFT_MID, 280, 0);

    s_rec_label = mk_label("", 0xe05050);        // voice input indicator
    lv_obj_align(s_rec_label, LV_ALIGN_LEFT_MID, 330, 0);

    // center: clock
    s_clock_label = mk_label("--:--", 0xffffff);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);

    // right: wifi / ip / temp / battery in one label, right-aligned
    s_right_label = mk_label("", 0xb0b0b0);
    lv_obj_align(s_right_label, LV_ALIGN_RIGHT_MID, 0, 0);

    update_clock();
    update_ssh();
    update_right();

    lv_timer_create(tick_cb, 1000, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "bar %dpx tall, terminal %d rows below", TAB5_STATUS_BAR_H, TERM_ROWS);
}

LV_FONT_DECLARE(cjk24);

void status_bar_set_rec(const char *txt)
{
    if (!s_rec_label) return;
    bsp_display_lock(0);
    lv_label_set_text(s_rec_label, txt ? txt : "");
    bsp_display_unlock();
}

void status_bar_set_ime(const char *txt)
{
    if (!s_ime_label) return;
    if (!txt) txt = "EN";
    // montserrat has no CJK glyphs - switch fonts for the 中 indicator
    const lv_font_t *font = &lv_font_montserrat_14;
    if ((unsigned char)txt[0] >= 0x80) {
        const lv_font_t *cjk = assets_font_cjk();
        font = cjk ? cjk : &cjk24;
    }
    bsp_display_lock(0);
    lv_obj_set_style_text_font(s_ime_label, font, 0);
    lv_label_set_text(s_ime_label, txt);
    bsp_display_unlock();
}
