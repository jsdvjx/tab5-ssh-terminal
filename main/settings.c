#include "settings.h"

#include <string.h>
#include <stddef.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
#define NS "tab5cfg"
#define KEY_BLOB "cfg_v2"

static void seed_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->sleep_timeout_s = 180;   // PM_IDLE_TIMEOUT_S; 0 = never sleep
    strlcpy(s->voice_url, "http://192.168.2.165:8765/inference",
            sizeof(s->voice_url));
    // T5NAT reverse tunnel: off by default, dials the public relay when on.
    s->nat_enabled = false;
    strlcpy(s->nat_url, "wss://t5.cc.hn/nat", sizeof(s->nat_url));
    strlcpy(s->wifi_ssid, CONFIG_TAB5_WIFI_SSID, sizeof(s->wifi_ssid));
    strlcpy(s->wifi_pass, CONFIG_TAB5_WIFI_PASSWORD, sizeof(s->wifi_pass));
    if (strlen(CONFIG_TAB5_SSH_HOST) > 0 && strcmp(CONFIG_TAB5_SSH_HOST, "192.168.1.100") != 0) {
        ssh_target_t *t = &s->targets[0];
        strlcpy(t->name, CONFIG_TAB5_SSH_HOST, sizeof(t->name));
        strlcpy(t->host, CONFIG_TAB5_SSH_HOST, sizeof(t->host));
        t->port = CONFIG_TAB5_SSH_PORT;
        strlcpy(t->user, CONFIG_TAB5_SSH_USER, sizeof(t->user));
        strlcpy(t->pass, CONFIG_TAB5_SSH_PASSWORD, sizeof(t->pass));
        strlcpy(t->cmd, CONFIG_TAB5_SSH_COMMAND, sizeof(t->cmd));
        s->n_targets = 1;
    }
}

bool settings_load(settings_t *s)
{
    seed_defaults(s);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no NVS namespace yet, using defaults");
        return false;
    }
    size_t len = sizeof(*s);
    esp_err_t err = nvs_get_blob(h, KEY_BLOB, s, &len);
    nvs_close(h);

    // Accept older (shorter) blobs: nvs_get_blob only overwrites the first
    // `len` bytes, so fields appended since (sleep_timeout_s) keep their
    // seeded defaults. Oversized/failed reads fall back to defaults.
    if (err != ESP_OK || len > sizeof(*s) || len < offsetof(settings_t, sleep_timeout_s)) {
        ESP_LOGI(TAG, "no saved config (err=%d len=%u), using defaults", err, (unsigned)len);
        seed_defaults(s);   // partial read protection
        return false;
    }
    if (s->n_targets < 0) s->n_targets = 0;
    if (s->n_targets > MAX_SSH_TARGETS) s->n_targets = MAX_SSH_TARGETS;
    if (s->last_target < 0 || s->last_target >= s->n_targets) s->last_target = 0;
    // Blob predating voice_url (or saved with it empty): keep the default.
    s->voice_url[sizeof(s->voice_url) - 1] = 0;
    if (!s->voice_url[0]) {
        strlcpy(s->voice_url, "http://192.168.2.165:8765/inference",
                sizeof(s->voice_url));
    }
    // Blob predating target_os[] keeps the seeded zeros (= generic server);
    // clamp anything out of range from a future/corrupt blob.
    for (int i = 0; i < MAX_SSH_TARGETS; i++) {
        if (s->target_os[i] >= TARGET_OS_COUNT) s->target_os[i] = TARGET_OS_SERVER;
        if (s->target_auth[i] >= TARGET_AUTH_COUNT) s->target_auth[i] = TARGET_AUTH_AUTO;
    }
    // Saved Wi-Fi networks: clamp the count, then migrate the legacy single
    // wifi_ssid/wifi_pass into wifi_nets[0] so the user's current network
    // survives the upgrade and appears in 已存网络.
    if (s->n_wifi_nets < 0) s->n_wifi_nets = 0;
    if (s->n_wifi_nets > MAX_WIFI_NETS) s->n_wifi_nets = MAX_WIFI_NETS;
    if (s->n_wifi_nets == 0 && s->wifi_ssid[0]) {
        strlcpy(s->wifi_nets[0].ssid, s->wifi_ssid, sizeof(s->wifi_nets[0].ssid));
        strlcpy(s->wifi_nets[0].pass, s->wifi_pass, sizeof(s->wifi_nets[0].pass));
        s->n_wifi_nets = 1;
        ESP_LOGI(TAG, "migrated legacy wifi '%s' -> wifi_nets[0]", s->wifi_ssid);
    }
    // Blob predating the nat_* tail (shorter read) keeps the seeded defaults,
    // but a blob saved with an empty nat_url should still get the default.
    s->nat_url[sizeof(s->nat_url) - 1] = 0;
    s->nat_token[sizeof(s->nat_token) - 1] = 0;
    s->nat_sub[sizeof(s->nat_sub) - 1] = 0;
    if (!s->nat_url[0]) {
        strlcpy(s->nat_url, "wss://t5.cc.hn/nat", sizeof(s->nat_url));
    }
    ESP_LOGI(TAG, "loaded: wifi='%s' targets=%d nets=%d ble=%d",
             s->wifi_ssid, s->n_targets, s->n_wifi_nets, s->ble_enabled);
    return true;
}

void settings_save(const settings_t *s)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, KEY_BLOB, s, sizeof(*s)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "saved: wifi='%s' targets=%d", s->wifi_ssid, s->n_targets);
}
