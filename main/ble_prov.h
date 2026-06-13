// BLE Wi-Fi provisioning (NimBLE GATT server over esp_hosted VHCI -> C6).
//
// Device name "Tab5-SSH", one primary service:
//   5a7e0001-91f4-4b9a-aa44-57c5b5e4d50a  service
//   5a7e0002-...  WIFI_SCAN   read+notify: JSON [{"s":"ssid","r":-55,"a":1}]
//                 reading it kicks an async scan; result arrives chunked over
//                 notifications, terminated by a zero-length notification.
//   5a7e0003-...  WIFI_CREDS  write: JSON {"s":"ssid","p":"password"}
//                 (long/queued writes supported; saved to NVS via settings.c)
//   5a7e0004-...  STATUS      read+notify: {"st":"idle|connecting|connected|
//                 fail","ip":"..."} — notified on every transition. Key
//                 import results arrive here too: {"key":"imported"} or
//                 {"key":"key_error: ..."}.
//   5a7e0005-...  KEY_UPLOAD  read: the device's OpenSSH public line
//                 ("ssh-rsa ... tab5", or "generating"/"none").
//                 write: SSH private key PEM, RSA only. Send in any chunking
//                 (each write appends; a chunk starting "-----BEGIN" resets
//                 the buffer; processing starts once "-----END ...PRIVATE
//                 KEY-----" has arrived). Result via STATUS notify.
//
// Lifecycle: advertising whenever Wi-Fi is not connected; stays on for a
// 5-minute grace period after got-IP, then stops. If the BT stack cannot be
// brought up (e.g. co-processor firmware without BT) init logs a warning and
// becomes a no-op — boot is never broken.
//
// Security: Just Works, no bonding. Credentials cross the air in plaintext.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "settings.h"

typedef enum {
    BLE_PROV_WIFI_IDLE = 0,
    BLE_PROV_WIFI_CONNECTING,
    BLE_PROV_WIFI_CONNECTED,
    BLE_PROV_WIFI_FAIL,
} ble_prov_wifi_state_t;

// Bring up NimBLE on the hosted VHCI and start advertising. `settings` must
// outlive the BLE stack (app_main's static). `creds_cb` fires (from the BLE
// host task; keep it quick) after credentials were written + saved to NVS.
// Never fails the boot: returns ESP_FAIL after logging a warning instead.
esp_err_t ble_prov_init(settings_t *settings, void (*creds_cb)(void));

// Manual advertising control (also driven automatically by Wi-Fi state).
void ble_prov_start(void);
void ble_prov_stop(void);

// True while the stack is up and actually advertising (shell `ble`, home panel).
bool ble_prov_is_advertising(void);

// Master enable/disable for BLE provisioning. When disabled, advertising is
// stopped and won't auto-restart on Wi-Fi events (the NimBLE host stays idle);
// when enabled, advertising resumes if Wi-Fi is not connected. The flag is
// persisted by the caller in settings (settings_t.ble_enabled); pass the boot
// value via ble_prov_init's settings so it is honored at startup.
void ble_prov_set_enabled(bool en);
bool ble_prov_is_enabled(void);

// Hook called from wifi.c on connection state transitions. `ip` may be NULL.
void ble_prov_wifi_state(ble_prov_wifi_state_t st, const char *ip);
