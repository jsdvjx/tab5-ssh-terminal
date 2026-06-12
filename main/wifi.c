// Wi-Fi management. esp_wifi_remote proxies the standard esp_wifi API to the
// ESP32-C6 over SDIO, so scan/connect code looks like any ESP32 station.

#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "bsp/m5stack_tab5.h"
#include "sdkconfig.h"

#include "ui_panel.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define MAX_RETRY     6

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static int s_retries;
static volatile bool s_want_connect;   // suppress auto-reconnect until creds set
static char s_ssid[33];

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want_connect) return;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        if (s_retries++ < MAX_RETRY) {
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retries, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_init(void)
{
    s_events = xEventGroupCreate();

    // The C6 sits behind a power switch on IO expander #1 — without this it
    // never enumerates on SDIO (endless sdmmc_card_init failures).
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    vTaskDelay(pdMS_TO_TICKS(200));     // let the C6 power up and boot

    // esp_hosted auto-init is patched out (see managed_components/.../port_
    // esp_hosted_host_init.c): at constructor time the startup-stack RAM is
    // not yet in the heap and the SDIO mempool allocation fails. Here the
    // full heap is available; the call is idempotent.
    extern int esp_hosted_init(void);
    ESP_ERROR_CHECK(esp_hosted_init() == 0 ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

int wifi_scan(wifi_ap_record_t *recs, int max)
{
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return -1;
    ESP_LOGI(TAG, "scan found %u APs", n);
    return n;
}

esp_err_t wifi_connect_blocking(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);
    s_retries = 0;
    s_want_connect = true;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if (bits & CONNECTED_BIT) return ESP_OK;
    s_want_connect = false;
    return ESP_FAIL;
}

bool wifi_get_ip(char *buf, size_t len)
{
    if (!s_netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) return false;
    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}

static void status_task(void *arg)
{
    while (true) {
        wifi_ap_record_t ap;
        char ip[20];
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && wifi_get_ip(ip, sizeof(ip))) {
            ui_panel_set_wifi((const char *)ap.ssid, ip, ap.rssi);
        } else {
            ui_panel_set_wifi(NULL, NULL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void wifi_start_status_updates(void)
{
    xTaskCreate(status_task, "wifi_status", 4096, NULL, 3, NULL);
}
