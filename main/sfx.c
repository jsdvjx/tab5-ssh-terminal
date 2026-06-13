// Tiny procedural sound effects for the boot animation (and future UI use).
// All samples are synthesized at init — no audio assets. Playback runs in its
// own task so callers (LVGL timers, key handlers) never block on I2S.
#include "sfx.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

static const char *TAG = "sfx";

#define SR 16000

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_q;

static int16_t *s_pcm[SFX_COUNT];
static size_t s_len[SFX_COUNT];      // samples

// xorshift PRNG (Math.random-free, deterministic)
static uint32_t s_rng = 0x12345678;
static inline float frand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (int32_t)s_rng / 2147483648.0f;
}

static int16_t *synth(size_t n) { return calloc(n, sizeof(int16_t)); }

// Typewriter click: 10ms of decaying band-ish noise with a thump.
static void make_click(int idx)
{
    size_t n = SR * 12 / 1000;
    int16_t *p = synth(n);
    float lp = 0;
    for (size_t i = 0; i < n; i++) {
        float env = expf(-(float)i / (SR * 0.0025f));
        lp += 0.35f * (frand() - lp);                 // soften the hiss
        float v = lp * env + 0.4f * sinf(2 * M_PI * 180 * i / SR) * env;
        p[i] = (int16_t)(v * 14000);
    }
    s_pcm[idx] = p; s_len[idx] = n;
}

// Shuffle tick: shorter, brighter.
static void make_tick(int idx)
{
    size_t n = SR * 8 / 1000;
    int16_t *p = synth(n);
    for (size_t i = 0; i < n; i++) {
        float env = expf(-(float)i / (SR * 0.0015f));
        float v = sinf(2 * M_PI * 2200 * i / SR) * env;
        p[i] = (int16_t)(v * 9000);
    }
    s_pcm[idx] = p; s_len[idx] = n;
}

// Settle ding: A5 -> E6 two-tone with a soft tail, ~220ms.
static void make_ding(int idx)
{
    size_t n = SR * 220 / 1000;
    int16_t *p = synth(n);
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = (t < 0.07f) ? 880.0f : 1318.5f;
        float env = (t < 0.07f) ? expf(-t / 0.05f)
                                : expf(-(t - 0.07f) / 0.09f);
        float v = sinf(2 * M_PI * f * t) * env * 0.8f
                + sinf(2 * M_PI * f * 2 * t) * env * 0.15f;   // 2nd harmonic
        p[i] = (int16_t)(v * 12000);
    }
    s_pcm[idx] = p; s_len[idx] = n;
}

static void sfx_task(void *arg)
{
    int id;
    while (xQueueReceive(s_q, &id, portMAX_DELAY) == pdTRUE) {
        if (id < 0 || id >= SFX_COUNT || !s_pcm[id]) continue;
        esp_codec_dev_write(s_spk, s_pcm[id], s_len[id] * sizeof(int16_t));
    }
}

void sfx_init(void)
{
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGW(TAG, "speaker init failed - sfx disabled");
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SR, .channel = 1, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec open failed - sfx disabled");
        s_spk = NULL;
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, 70);

    make_click(SFX_CLICK);
    make_tick(SFX_TICK);
    make_ding(SFX_DING);

    s_q = xQueueCreate(8, sizeof(int));
    xTaskCreate(sfx_task, "sfx", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready");
}

void sfx_play(int id)
{
    if (!s_spk || !s_q) return;
    xQueueSend(s_q, &id, 0);
}
