#include "settings.h"

#include <string.h>

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

    if (err != ESP_OK || len != sizeof(*s)) {
        ESP_LOGI(TAG, "no saved config (err=%d len=%u), using defaults", err, (unsigned)len);
        seed_defaults(s);   // partial read protection
        return false;
    }
    if (s->n_targets < 0) s->n_targets = 0;
    if (s->n_targets > MAX_SSH_TARGETS) s->n_targets = MAX_SSH_TARGETS;
    if (s->last_target < 0 || s->last_target >= s->n_targets) s->last_target = 0;
    ESP_LOGI(TAG, "loaded: wifi='%s' targets=%d", s->wifi_ssid, s->n_targets);
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
