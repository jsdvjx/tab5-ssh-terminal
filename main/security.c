#include "security.h"

#include <string.h>

#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "security";
#define NS      "tab5sec"
#define KEY_PIN "pin"

// Unambiguous alphabet: no 0/O, no 1/I/L.
static const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define ALPHABET_N (sizeof(ALPHABET) - 1)   // 32

static char s_pin[SECURITY_PIN_LEN + 1];

static void gen_pin(char *out)
{
    for (int i = 0; i < SECURITY_PIN_LEN; i++) {
        out[i] = ALPHABET[esp_random() % ALPHABET_N];
    }
    out[SECURITY_PIN_LEN] = 0;
}

static void persist_pin(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed; PIN not persisted");
        return;
    }
    nvs_set_str(h, KEY_PIN, s_pin);
    nvs_commit(h);
    nvs_close(h);
}

void security_init(void)
{
    nvs_handle_t h;
    bool have = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_pin);
        if (nvs_get_str(h, KEY_PIN, s_pin, &len) == ESP_OK
            && strlen(s_pin) == SECURITY_PIN_LEN) {
            have = true;
        }
        nvs_close(h);
    }
    if (!have) {
        gen_pin(s_pin);
        persist_pin();
        ESP_LOGI(TAG, "generated new device PIN");
    }
    ESP_LOGI(TAG, "device PIN: %s", s_pin);
}

const char *security_pin(void)
{
    return s_pin;
}

void security_regen_pin(void)
{
    gen_pin(s_pin);
    persist_pin();
    ESP_LOGI(TAG, "device PIN regenerated: %s", s_pin);
}

bool security_check(const char *candidate)
{
    if (!candidate) return false;
    // Constant-time-ish: compare over the full PIN length regardless of where
    // the first mismatch is, and fold in a length check.
    size_t clen = strlen(candidate);
    unsigned diff = (unsigned)(clen ^ (size_t)SECURITY_PIN_LEN);
    for (int i = 0; i < SECURITY_PIN_LEN; i++) {
        char cc = (i < (int)clen) ? candidate[i] : 0;
        diff |= (unsigned)(cc ^ s_pin[i]);
    }
    return diff == 0;
}
