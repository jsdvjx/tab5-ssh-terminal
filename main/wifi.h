#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi.h"

// Powers the C6 (IO expander), runs the deferred esp_hosted_init(), brings up
// netif + Wi-Fi in STA mode. Call once, before scan/connect.
esp_err_t wifi_init(void);

// Blocking scan; fills up to `max` records, returns count (<0 on error).
int wifi_scan(wifi_ap_record_t *recs, int max);

// Connect to an AP; blocks until got IP or retries exhausted.
esp_err_t wifi_connect_blocking(const char *ssid, const char *password);

// Current IP as string; false if not connected.
bool wifi_get_ip(char *buf, size_t len);

// Background task: refresh the side panel Wi-Fi block every few seconds.
void wifi_start_status_updates(void);
