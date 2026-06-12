#include "setup.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "wifi.h"
#include "ui_panel.h"

static const char *TAG = "setup";

static QueueHandle_t s_keys;   // single bytes from the keyboard

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
static void read_line(term_t *t, const char *label, char *out, size_t cap, bool mask)
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
    int pick = ui_panel_pick("Select Wi-Fi network", names, uniq);
    strlcpy(s->wifi_ssid, names[pick], sizeof(s->wifi_ssid));
    ESP_LOGI(TAG, "picked SSID '%s'", s->wifi_ssid);

    char line[80];
    snprintf(line, sizeof(line), "[tab5] network: %s\r\n", s->wifi_ssid);
    echo(t, line);
    s->wifi_pass[0] = 0;
    read_line(t, "Wi-Fi password", s->wifi_pass, sizeof(s->wifi_pass), true);

    settings_save(s);
    return true;
}
