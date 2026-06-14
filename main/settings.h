// Runtime settings, persisted in NVS as a versioned blob.
// Holds Wi-Fi credentials and up to MAX_SSH_TARGETS SSH servers.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MAX_SSH_TARGETS 8
#define MAX_WIFI_NETS   8

// A saved Wi-Fi credential pair (settings_t.wifi_nets[]). The "active" pair
// wifi.c actually uses is still wifi_ssid/wifi_pass; connecting to a saved net
// copies it into the active pair.
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

// Per-target OS tag (icon in the SSH device grid). Stored OUTSIDE
// ssh_target_t (settings_t.target_os[]) so the saved-blob stride of
// targets[] never changes — old NVS blobs load untouched.
typedef enum {
    TARGET_OS_SERVER = 0,    // default: generic server
    TARGET_OS_APPLE,
    TARGET_OS_TUX,
    TARGET_OS_UBUNTU,
    TARGET_OS_DEBIAN,
    TARGET_OS_WINDOWS,
    TARGET_OS_RASPBERRY,
    TARGET_OS_COUNT,
} target_os_t;

// Per-target SSH auth method (settings_t.target_auth[], outside ssh_target_t
// to keep the targets[] blob stride stable). AUTO = device key first then
// password (the original behaviour, and the default for old blobs = 0).
typedef enum {
    TARGET_AUTH_AUTO = 0,    // publickey (device key) then password
    TARGET_AUTH_PASSWORD,    // password only
    TARGET_AUTH_KEY,         // device key only (证书)
    TARGET_AUTH_COUNT,
} target_auth_t;

typedef struct {
    char name[24];      // display name for the panel / picker
    char host[64];
    int  port;
    char user[32];
    char pass[64];
    char cmd[128];      // run after login; empty = interactive shell
} ssh_target_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    ssh_target_t targets[MAX_SSH_TARGETS];
    int  n_targets;
    int  last_target;   // index to preselect on boot
    uint16_t sleep_timeout_s;   // screen sleep idle timeout; 0 = never
    char voice_url[96];         // whisper server endpoint for voice input
    // Appended fields only below this line: settings_load() accepts shorter
    // (older) blobs and keeps the seeded defaults for the missing tail.
    uint8_t target_os[MAX_SSH_TARGETS];   // target_os_t per targets[i]
    uint8_t target_auth[MAX_SSH_TARGETS]; // target_auth_t per targets[i]
    // Saved Wi-Fi networks. The legacy single wifi_ssid/wifi_pass is migrated
    // into wifi_nets[0] on first load (see settings_load).
    wifi_cred_t wifi_nets[MAX_WIFI_NETS];
    int  n_wifi_nets;
    bool ble_enabled;                     // BLE master on/off; default OFF
                                          // (fresh/old zero blob = disabled)
    uint8_t lang;                         // UI language (lang_t); default 0 =
                                          // LANG_ZH. Appended last: old blobs
                                          // (shorter reads) keep the seeded 0.
    // T5NAT reverse-tunnel (main/nat_tunnel.c). Appended after lang; old blobs
    // (shorter reads) keep the seeded defaults below — same tail-seed pattern.
    bool nat_enabled;                     // dial the relay on boot; default OFF
    char nat_url[80];                     // relay WSS endpoint; default below
    char nat_token[48];                   // device auth token (opaque)
    char nat_sub[32];                     // desired subdomain label
} settings_t;

// Loads from NVS. Returns true if a saved config existed; on first boot the
// struct is seeded from the menuconfig defaults (a target is created from
// CONFIG_TAB5_SSH_* if a host is set).
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);
