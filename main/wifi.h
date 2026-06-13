#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "settings.h"

// Powers the C6 (IO expander), runs the deferred esp_hosted_init(), brings up
// netif + Wi-Fi in STA mode. Call once, before scan/connect.
esp_err_t wifi_init(void);

// Blocking scan; fills up to `max` records, returns count (<0 on error).
int wifi_scan(wifi_ap_record_t *recs, int max);

// Connect to an AP; blocks until got IP or retries exhausted.
esp_err_t wifi_connect_blocking(const char *ssid, const char *password);

// Current IP as string; false if not connected.
bool wifi_get_ip(char *buf, size_t len);

// True between GOT_IP and disconnect (station has a working link + IP).
bool wifi_is_connected(void);

// Connect to saved network wifi_nets[idx]: copies it into the active pair
// (wifi_ssid/wifi_pass), saves settings, then reconnects on a background task
// so the caller (LVGL thread) never blocks. Safe to call from the LVGL task.
void wifi_connect_saved_async(settings_t *s, int idx);

// Background task: refresh the side panel Wi-Fi block every few seconds.
void wifi_start_status_updates(void);
