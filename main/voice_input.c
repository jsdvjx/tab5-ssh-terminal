// Voice input pipeline: ES7210 mic -> PSRAM PCM ring -> WAV over HTTP to a
// whisper server -> transcription typed into the SSH session.
//
// Threads: voice_input_toggle()/_cancel() run on the HID/I2C-keyboard tasks
// and only post to a queue. All mic + HTTP work happens on the worker task,
// so the keyboard path never blocks. voice_input_record_wav() (httpd task,
// `voice test` shell task) shares the mic and capture buffer with the worker
// under s_mic_mu.
//
// The mic shares the I2S bus with the speaker (sfx.c). bsp_audio.c sets the
// port up full-duplex (one tx + one rx channel, same clock); sfx opens the
// speaker at 16k/16/mono and we open the mic with the same sample config, so
// the shared BCLK/WS never needs to change and duplex just works.

#include "voice_input.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#include "ime_filter.h"
#include "status_bar.h"
#include "sfx.h"

static const char *TAG = "voice";

#define VI_SR           16000
#define VI_MAX_S        30                      // recording cap
#define VI_MAX_SAMPLES  (VI_SR * VI_MAX_S)      // 480k samples = 960 KB
#define VI_CHUNK        (VI_SR / 10)            // 100 ms read granularity
#define VI_TEXT_MAX     1024
#define WAV_HDR_LEN     44

typedef enum { VC_TOGGLE, VC_CANCEL } vi_cmd_t;

static settings_t *s_cfg;
static QueueHandle_t s_q;
static SemaphoreHandle_t s_mic_mu;   // mic codec + s_buf owner
static esp_codec_dev_handle_t s_mic;
static int s_mic_ch;                 // channels the codec was opened with

static int16_t *s_buf;               // PSRAM capture buffer (mono)
static int16_t *s_chunk;             // PSRAM read staging (s_mic_ch channels)
static size_t s_samples;
static volatile bool s_recording;

// ------------------------------------------------------------------- mic

// Lazy codec open. ES7210 may insist on multi-channel slots; try mono first
// and fall back to 2ch (we keep channel 0 when downmixing).
static bool mic_open(void)
{
    if (s_mic) return true;

    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) {
        ESP_LOGE(TAG, "mic codec init failed");
        return false;
    }
    for (int ch = 1; ch <= 2; ch++) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = VI_SR, .channel = ch, .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK) {
            s_mic_ch = ch;
            break;
        }
    }
    if (!s_mic_ch) {
        ESP_LOGE(TAG, "mic codec open failed");
        return false;
    }
    esp_codec_dev_set_in_gain(mic, 30.0f);

    s_buf = heap_caps_malloc(VI_MAX_SAMPLES * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_chunk = heap_caps_malloc(VI_CHUNK * s_mic_ch * sizeof(int16_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !s_chunk) {
        ESP_LOGE(TAG, "capture buffer alloc failed");
        free(s_buf); s_buf = NULL;
        free(s_chunk); s_chunk = NULL;
        return false;
    }
    s_mic = mic;
    ESP_LOGI(TAG, "mic open: %dkHz 16-bit %dch", VI_SR / 1000, s_mic_ch);
    return true;
}

// Read up to VI_CHUNK mono samples into s_buf + s_samples. Caller holds
// s_mic_mu. Returns false when the buffer is full or the read fails.
static bool capture_chunk(void)
{
    size_t want = VI_CHUNK;
    if (s_samples + want > VI_MAX_SAMPLES) want = VI_MAX_SAMPLES - s_samples;
    if (want == 0) return false;

    int rc = esp_codec_dev_read(s_mic, s_chunk,
                                want * s_mic_ch * sizeof(int16_t));
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic read failed (%d)", rc);
        return false;
    }
    if (s_mic_ch == 1) {
        memcpy(s_buf + s_samples, s_chunk, want * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < want; i++) {     // keep channel 0
            s_buf[s_samples + i] = s_chunk[i * s_mic_ch];
        }
    }
    s_samples += want;
    return s_samples < VI_MAX_SAMPLES;
}

// --------------------------------------------------------------------- wav

static void wav_header(uint8_t *h, uint32_t pcm_bytes)
{
    uint32_t u32;
    uint16_t u16;
    memcpy(h, "RIFF", 4);
    u32 = 36 + pcm_bytes;          memcpy(h + 4, &u32, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    u32 = 16;                      memcpy(h + 16, &u32, 4);   // fmt size
    u16 = 1;                       memcpy(h + 20, &u16, 2);   // PCM
    u16 = 1;                       memcpy(h + 22, &u16, 2);   // mono
    u32 = VI_SR;                   memcpy(h + 24, &u32, 4);
    u32 = VI_SR * 2;               memcpy(h + 28, &u32, 4);   // byte rate
    u16 = 2;                       memcpy(h + 32, &u16, 2);   // block align
    u16 = 16;                      memcpy(h + 34, &u16, 2);   // bits
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &pcm_bytes, 4);
}

// ------------------------------------------------------------------ upload

// POST the capture as multipart/form-data to the whisper server and pull the
// "text" field out of the JSON reply. Matches whisper.cpp's whisper-server
// /inference API. Returns false on any transport/parse error.
static bool transcribe(const int16_t *pcm, size_t samples,
                       char *text, size_t text_cap)
{
    const char *url = (s_cfg && s_cfg->voice_url[0]) ? s_cfg->voice_url
                      : "http://192.168.2.165:8765/inference";
    static const char *BOUND = "tab5voiceboundary7391";
    char head[256], mid[192], tail[64];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n"
        "\r\njson\r\n", BOUND);
    int mid_len = snprintf(mid, sizeof(mid),
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"rec.wav\"\r\nContent-Type: audio/wav\r\n\r\n", BOUND);
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", BOUND);

    uint8_t hdr[WAV_HDR_LEN];
    uint32_t pcm_bytes = samples * sizeof(int16_t);
    wav_header(hdr, pcm_bytes);

    size_t body_len = head_len + mid_len + WAV_HDR_LEN + pcm_bytes + tail_len;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,        // base model on CPU can take a while
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", BOUND);
    esp_http_client_set_header(cli, "Content-Type", ctype);

    bool ok = false;
    if (esp_http_client_open(cli, body_len) != ESP_OK) {
        ESP_LOGE(TAG, "connect to %s failed", url);
        goto out;
    }
    if (esp_http_client_write(cli, head, head_len) < 0 ||
        esp_http_client_write(cli, mid, mid_len) < 0 ||
        esp_http_client_write(cli, (const char *)hdr, WAV_HDR_LEN) < 0 ||
        esp_http_client_write(cli, (const char *)pcm, pcm_bytes) < 0 ||
        esp_http_client_write(cli, tail, tail_len) < 0) {
        ESP_LOGE(TAG, "upload write failed");
        goto out;
    }
    if (esp_http_client_fetch_headers(cli) < 0) goto out;

    int status = esp_http_client_get_status_code(cli);
    char resp[2048];
    int rlen = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
    if (rlen < 0) rlen = 0;
    resp[rlen] = 0;
    if (status != 200) {
        ESP_LOGE(TAG, "server %d: %.120s", status, resp);
        goto out;
    }

    cJSON *root = cJSON_Parse(resp);
    cJSON *t = root ? cJSON_GetObjectItem(root, "text") : NULL;
    if (t && cJSON_IsString(t)) {
        strlcpy(text, t->valuestring, text_cap);
        ok = true;
    } else {
        ESP_LOGE(TAG, "no text in reply: %.120s", resp);
    }
    if (root) cJSON_Delete(root);

out:
    esp_http_client_cleanup(cli);
    return ok;
}

// Whisper pads with leading spaces and trailing newlines — strip both ends.
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// ------------------------------------------------------------------ worker

static void rec_failed(const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    status_bar_set_rec("ERR");
    vTaskDelay(pdMS_TO_TICKS(1500));
    status_bar_set_rec(NULL);
}

static void vi_task(void *arg)
{
    static char text[VI_TEXT_MAX];
    vi_cmd_t cmd;

    while (xQueueReceive(s_q, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd != VC_TOGGLE) continue;          // stale cancel

        if (xSemaphoreTake(s_mic_mu, 0) != pdTRUE) {
            ESP_LOGW(TAG, "mic busy (/api/rec or voice test running)");
            continue;
        }
        if (!mic_open()) {
            xSemaphoreGive(s_mic_mu);
            rec_failed("mic unavailable");
            continue;
        }

        s_samples = 0;
        s_recording = true;
        sfx_play(SFX_TICK);
        status_bar_set_rec("REC");

        bool cancelled = false;
        bool run = true;
        while (run) {
            if (!capture_chunk()) run = false;   // full / error
            while (xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
                run = false;
                if (cmd == VC_CANCEL) cancelled = true;
            }
        }
        s_recording = false;
        size_t samples = s_samples;
        sfx_play(SFX_TICK);

        if (cancelled || samples < VI_SR / 4) {  // <250 ms: nothing to say
            xSemaphoreGive(s_mic_mu);
            status_bar_set_rec(NULL);
            continue;
        }

        status_bar_set_rec("...");               // uploading/transcribing
        ESP_LOGI(TAG, "uploading %u samples (%.1fs)",
                 (unsigned)samples, (float)samples / VI_SR);
        bool ok = transcribe(s_buf, samples, text, sizeof(text));
        xSemaphoreGive(s_mic_mu);

        if (!ok) {
            rec_failed("transcription failed");
            continue;
        }
        char *t = trim(text);
        status_bar_set_rec(NULL);
        if (t[0]) {
            ESP_LOGI(TAG, "-> \"%s\"", t);
            ime_filter_commit_text(t);
            sfx_play(SFX_DING);
        } else {
            ESP_LOGW(TAG, "empty transcription");
        }
    }
}

// --------------------------------------------------------------- public API

void voice_input_init(settings_t *s)
{
    s_cfg = s;
    s_q = xQueueCreate(4, sizeof(vi_cmd_t));
    s_mic_mu = xSemaphoreCreateMutex();
    // HTTP client + cJSON on this stack; keep it roomy.
    xTaskCreate(vi_task, "voice", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "ready — Ctrl+Alt+V");
}

void voice_input_toggle(void)
{
    if (!s_q) return;
    vi_cmd_t cmd = VC_TOGGLE;
    xQueueSend(s_q, &cmd, 0);
}

void voice_input_cancel(void)
{
    if (!s_q || !s_recording) return;
    vi_cmd_t cmd = VC_CANCEL;
    xQueueSend(s_q, &cmd, 0);
}

bool voice_input_is_recording(void)
{
    return s_recording;
}

bool voice_input_record_wav(int ms, uint8_t **out, size_t *out_len)
{
    if (!s_mic_mu || ms <= 0) return false;
    if (xSemaphoreTake(s_mic_mu, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    if (!mic_open()) {
        xSemaphoreGive(s_mic_mu);
        return false;
    }

    size_t want = (size_t)ms * VI_SR / 1000;
    if (want > VI_MAX_SAMPLES) want = VI_MAX_SAMPLES;
    s_samples = 0;
    while (s_samples < want) {
        if (!capture_chunk() && s_samples < want) {
            if (s_samples == 0) {                // hard read error up front
                xSemaphoreGive(s_mic_mu);
                return false;
            }
            break;
        }
    }
    size_t samples = s_samples < want ? s_samples : want;

    size_t pcm_bytes = samples * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(WAV_HDR_LEN + pcm_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        xSemaphoreGive(s_mic_mu);
        return false;
    }
    wav_header(wav, pcm_bytes);
    memcpy(wav + WAV_HDR_LEN, s_buf, pcm_bytes);
    xSemaphoreGive(s_mic_mu);

    *out = wav;
    *out_len = WAV_HDR_LEN + pcm_bytes;
    return true;
}

int voice_input_test(int ms)
{
    if (!s_mic_mu) { printf("voice input not initialized\n"); return 1; }
    if (xSemaphoreTake(s_mic_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("mic busy\n");
        return 1;
    }
    if (!mic_open()) {
        xSemaphoreGive(s_mic_mu);
        printf("mic unavailable\n");
        return 1;
    }

    printf("recording %dms...\n", ms);
    size_t want = (size_t)ms * VI_SR / 1000;
    if (want > VI_MAX_SAMPLES) want = VI_MAX_SAMPLES;
    s_samples = 0;
    while (s_samples < want && capture_chunk()) { }

    static char text[VI_TEXT_MAX];               // shell task only
    printf("uploading %u samples to %s...\n", (unsigned)s_samples,
           s_cfg->voice_url);
    bool ok = transcribe(s_buf, s_samples, text, sizeof(text));
    xSemaphoreGive(s_mic_mu);

    if (!ok) { printf("transcription failed (check the server / `voice` url)\n"); return 1; }
    printf("text: \"%s\"\n", trim(text));
    return 0;
}
