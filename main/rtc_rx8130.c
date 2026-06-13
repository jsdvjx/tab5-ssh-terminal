// RX8130CE RTC @ I2C 0x32. BCD time registers at 0x10:
//   sec, min, hour (24h), weekday (bit mask, 1<<wday), day, month, year (00-99).
// Boot: seed system time from the RTC if it looks sane; once SNTP syncs,
// write the corrected time back so the RTC stays accurate across power loss.

#include "rtc_rx8130.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "driver/i2c_master.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "rtc";

#define RX8130_ADDR     0x32
#define RX8130_REG_TIME 0x10

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static bool s_sntp_started;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0f); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtc_rx8130_read(struct tm *out)
{
    if (!s_dev) return false;
    uint8_t reg = RX8130_REG_TIME;
    uint8_t b[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 100) != ESP_OK) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2bin(b[0] & 0x7f);
    out->tm_min  = bcd2bin(b[1] & 0x7f);
    out->tm_hour = bcd2bin(b[2] & 0x3f);
    // b[3] is a weekday bit mask (one bit set, bit0 = Sunday)
    for (int i = 0; i < 7; i++) {
        if (b[3] & (1 << i)) { out->tm_wday = i; break; }
    }
    out->tm_mday = bcd2bin(b[4] & 0x3f);
    out->tm_mon  = bcd2bin(b[5] & 0x1f) - 1;
    out->tm_year = bcd2bin(b[6]) + 100;         // RTC stores 20xx
    return true;
}

bool rtc_rx8130_write(const struct tm *t)
{
    if (!s_dev) return false;
    uint8_t b[8] = {
        RX8130_REG_TIME,
        bin2bcd(t->tm_sec),
        bin2bcd(t->tm_min),
        bin2bcd(t->tm_hour),
        (uint8_t)(1 << t->tm_wday),
        bin2bcd(t->tm_mday),
        bin2bcd(t->tm_mon + 1),
        bin2bcd(t->tm_year % 100),
    };
    return i2c_master_transmit(s_dev, b, sizeof(b), 100) == ESP_OK;
}

static void sntp_synced_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP synced");
    struct tm tm_local;
    time_t now = tv->tv_sec;
    localtime_r(&now, &tm_local);
    if (rtc_rx8130_write(&tm_local)) {
        ESP_LOGI(TAG, "RTC updated: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                 tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    }
}

void rtc_rx8130_sntp_start(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_synced_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

bool rtc_rx8130_init(void)
{
    // Local timezone for clock display and RTC (RTC holds local time).
    setenv("TZ", "CST-8", 1);
    tzset();

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "no i2c bus");
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RX8130_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "add device failed");
        return false;
    }

    struct tm t;
    if (!rtc_rx8130_read(&t)) {
        ESP_LOGW(TAG, "RX8130 not responding — RTC disabled");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    s_present = true;

    if (t.tm_year + 1900 >= 2025) {
        time_t secs = mktime(&t);   // RTC holds local time; mktime honors TZ
        struct timeval tv = { .tv_sec = secs };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "system time set from RTC: %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC time implausible (%d) — waiting for SNTP", t.tm_year + 1900);
    }
    return s_present;
}
