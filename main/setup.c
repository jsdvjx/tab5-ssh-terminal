#include "setup.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "wifi.h"

static const char *TAG = "setup";

static QueueHandle_t s_keys;   // single bytes from the keyboard

// ------------------------------------------------------------ SSID picker
// Minimal self-contained modal list (the boot-phase ui_panel that used to
// provide this is gone). Blocks the calling task until a row is tapped.

static SemaphoreHandle_t s_pick_sem;
static volatile int s_pick_result;
static volatile bool s_pick_active;

static void pick_btn_cb(lv_event_t *e)
{
    s_pick_result = (int)(intptr_t)lv_event_get_user_data(e);
    xSemaphoreGive(s_pick_sem);
}

// Returns the tapped index, or -1 if cancelled (setup_cancel).
static int setup_pick(const char *title_text, const char *names[], int n)
{
    if (!s_pick_sem) s_pick_sem = xSemaphoreCreateBinary();

    bsp_display_lock(0);
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(modal, 500, 600);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x202030), 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_move_foreground(modal);

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

    s_pick_active = true;
    xSemaphoreTake(s_pick_sem, portMAX_DELAY);
    s_pick_active = false;

    bsp_display_lock(0);
    lv_obj_delete(modal);
    bsp_display_unlock();
    return s_pick_result;
}

static void setup_pick_cancel(void)
{
    if (!s_pick_active || !s_pick_sem) return;
    s_pick_result = -1;
    xSemaphoreGive(s_pick_sem);
}

// Injected by setup_cancel() (BLE provisioning) to abort blocking prompts.
#define SETUP_CANCEL_BYTE 0x04   // EOT; never produced by the keyboards

void setup_cancel(void)
{
    setup_pick_cancel();                // unblocks the SSID picker, if up
    if (s_keys) {
        uint8_t b = SETUP_CANCEL_BYTE;  // unblocks the password prompt
        xQueueSend(s_keys, &b, 0);
    }
}

void setup_key_input(const uint8_t *data, size_t len)
{
    if (!s_keys) return;
    for (size_t i = 0; i < len; i++) {
        xQueueSend(s_keys, &data[i], 0);
    }
}

static void ensure_queue(void)
{
    if (!s_keys) s_keys = xQueueCreate(64, 1);
    xQueueReset(s_keys);
}

static void echo(term_t *t, const char *s)
{
    term_feed(t, (const uint8_t *)s, strlen(s));
}

// Line editor on the terminal: printable ASCII, backspace; Enter accepts.
// Empty input keeps `out` unchanged (shown as default). mask=true echoes '*'.
// Returns false if aborted by setup_cancel() (BLE creds arrived).
static bool read_line(term_t *t, const char *label, char *out, size_t cap, bool mask)
{
    char prompt[192];
    if (mask) {
        snprintf(prompt, sizeof(prompt), "  %s [%s]: ", label,
                 out[0] ? "****" : "");
    } else {
        snprintf(prompt, sizeof(prompt), "  %s [%s]: ", label, out);
    }
    echo(t, prompt);

    char buf[128];
    size_t len = 0;
    while (true) {
        uint8_t b;
        if (xQueueReceive(s_keys, &b, portMAX_DELAY) != pdTRUE) continue;
        if (b == SETUP_CANCEL_BYTE) {
            echo(t, "\r\n");
            return false;
        }
        if (b == '\r' || b == '\n') {
            echo(t, "\r\n");
            break;
        }
        if (b == 0x7f || b == '\b') {
            if (len > 0) { len--; echo(t, "\b \b"); }
            continue;
        }
        if (b < 0x20 || b > 0x7e) continue;     // setup is plain ASCII
        if (len < sizeof(buf) - 1 && len < cap - 1) {
            buf[len++] = (char)b;
            char e[2] = { mask ? '*' : (char)b, 0 };
            echo(t, e);
        }
    }
    if (len > 0) {
        buf[len] = 0;
        strlcpy(out, buf, cap);
    }
    return true;
}

bool setup_offered(term_t *t, int timeout_ms)
{
    ensure_queue();
    echo(t, "[tab5] press 's' for Wi-Fi setup... ");
    uint8_t b;
    bool enter = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (xQueueReceive(s_keys, &b, pdMS_TO_TICKS(100)) == pdTRUE
            && (b == 's' || b == 'S')) {
            enter = true;
            break;
        }
    }
    echo(t, enter ? "yes\r\n" : "no\r\n");
    return enter;
}

bool setup_wifi(term_t *t, settings_t *s)
{
    ensure_queue();

    echo(t, "[tab5] scanning Wi-Fi networks...\r\n");
    static wifi_ap_record_t recs[24];
    int n = wifi_scan(recs, 24);
    if (n <= 0) {
        echo(t, "[tab5] scan found nothing\r\n");
        return false;
    }

    // Dedupe SSIDs (strongest first — scan results are RSSI-sorted already)
    const char *names[24];
    int uniq = 0;
    for (int i = 0; i < n && uniq < 24; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < uniq; j++) {
            if (strcmp(names[j], ssid) == 0) { dup = true; break; }
        }
        if (!dup) names[uniq++] = ssid;
    }

    echo(t, "[tab5] tap your network on the screen\r\n");
    int pick = setup_pick("Select Wi-Fi network", names, uniq);
    if (pick < 0) {
        // Cancelled by BLE provisioning: credentials already in *s + NVS.
        echo(t, "[tab5] credentials received over BLE\r\n");
        return s->wifi_ssid[0] != 0;
    }
    strlcpy(s->wifi_ssid, names[pick], sizeof(s->wifi_ssid));
    ESP_LOGI(TAG, "picked SSID '%s'", s->wifi_ssid);

    char line[80];
    snprintf(line, sizeof(line), "[tab5] network: %s\r\n", s->wifi_ssid);
    echo(t, line);
    s->wifi_pass[0] = 0;
    if (!read_line(t, "Wi-Fi password", s->wifi_pass, sizeof(s->wifi_pass), true)) {
        echo(t, "[tab5] credentials received over BLE\r\n");
        return s->wifi_ssid[0] != 0;    // BLE already wrote ssid/pass + NVS
    }

    settings_save(s);
    return true;
}
