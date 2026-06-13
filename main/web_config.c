#include "web_config.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "bsp/m5stack_tab5.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"

#include "lvgl.h"

#include "ssh_client.h"
#include "term_render.h"
#include "ui_home.h"
#include "ime_filter.h"
#include "power_mon.h"
#include "power_mgmt.h"
#include "voice_input.h"

static const char *TAG = "web_config";

static settings_t *s_cfg;
static void (*s_on_update)(void);

// Page embedded from main/web/index.html (EMBED_TXTFILES)
extern const char _binary_index_html_start[];
extern const char _binary_index_html_end[];

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, _binary_index_html_start,
                           _binary_index_html_end - _binary_index_html_start - 1);
}

static esp_err_t targets_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "targets");
    for (int i = 0; i < s_cfg->n_targets; i++) {
        const ssh_target_t *t = &s_cfg->targets[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "host", t->host);
        cJSON_AddNumberToObject(o, "port", t->port);
        cJSON_AddStringToObject(o, "user", t->user);
        cJSON_AddStringToObject(o, "cmd", t->cmd);
        cJSON_AddNumberToObject(o, "os", s_cfg->target_os[i]);
        // password intentionally omitted
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "active", s_cfg->last_target);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t len = req->content_len;
    if (len >= cap) return ESP_FAIL;
    size_t off = 0;
    while (off < len) {
        int r = httpd_req_recv(req, buf + off, len - off);
        if (r <= 0) return ESP_FAIL;
        off += r;
    }
    buf[len] = 0;
    return ESP_OK;
}

static const char *jstr(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : "";
}

static esp_err_t targets_post(httpd_req_t *req)
{
    static char body[8192];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *arr = cJSON_GetObjectItem(root, "targets");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no targets");
    }

    settings_t old = *s_cfg;   // for password carry-over
    int n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (n >= MAX_SSH_TARGETS) break;
        ssh_target_t *t = &s_cfg->targets[n];
        memset(t, 0, sizeof(*t));
        strlcpy(t->name, jstr(item, "name"), sizeof(t->name));
        strlcpy(t->host, jstr(item, "host"), sizeof(t->host));
        strlcpy(t->user, jstr(item, "user"), sizeof(t->user));
        strlcpy(t->cmd,  jstr(item, "cmd"),  sizeof(t->cmd));
        cJSON *port = cJSON_GetObjectItem(item, "port");
        t->port = (port && cJSON_IsNumber(port) && port->valueint > 0) ? port->valueint : 22;
        cJSON *os = cJSON_GetObjectItem(item, "os");
        if (os && cJSON_IsNumber(os) && os->valueint >= 0
            && os->valueint < TARGET_OS_COUNT) {
            s_cfg->target_os[n] = (uint8_t)os->valueint;
        } else {
            // older web UI without the field: keep the previous tag
            s_cfg->target_os[n] = TARGET_OS_SERVER;
            for (int i = 0; i < old.n_targets; i++) {
                if (strcmp(old.targets[i].host, t->host) == 0 &&
                    strcmp(old.targets[i].user, t->user) == 0) {
                    s_cfg->target_os[n] = old.target_os[i];
                    break;
                }
            }
        }

        const char *pass = jstr(item, "pass");
        if (pass[0]) {
            strlcpy(t->pass, pass, sizeof(t->pass));
        } else {
            // keep previous password for a matching host+user
            for (int i = 0; i < old.n_targets; i++) {
                if (strcmp(old.targets[i].host, t->host) == 0 &&
                    strcmp(old.targets[i].user, t->user) == 0) {
                    strlcpy(t->pass, old.targets[i].pass, sizeof(t->pass));
                    break;
                }
            }
        }
        if (t->host[0]) n++;
    }
    cJSON_Delete(root);

    s_cfg->n_targets = n;
    if (s_cfg->last_target >= n) s_cfg->last_target = 0;
    settings_save(s_cfg);
    if (s_on_update) s_on_update();

    ESP_LOGI(TAG, "targets updated: %d entries", n);
    return httpd_resp_sendstr(req, "ok");
}

// GET /debug — pipeline counters for hang diagnosis.
extern volatile uint32_t g_dbg_kbd_events, g_dbg_sink_bytes, g_dbg_ssh_rx,
                         g_dbg_ssh_tx, g_dbg_ssh_loops, g_dbg_term_feed,
                         g_dbg_render_ticks;
static esp_err_t debug_get(httpd_req_t *req)
{
    extern size_t heap_caps_get_free_size(uint32_t caps);
    char buf[768];
    int len = snprintf(buf, sizeof(buf),
             "{\"kbd_events\":%lu,\"sink_bytes\":%lu,\"ssh_rx\":%lu,"
             "\"ssh_tx\":%lu,\"ssh_loops\":%lu,\"term_feed\":%lu,"
             "\"render_ticks\":%lu,\"free_int\":%u,\"free_psram\":%u",
             (unsigned long)g_dbg_kbd_events, (unsigned long)g_dbg_sink_bytes,
             (unsigned long)g_dbg_ssh_rx, (unsigned long)g_dbg_ssh_tx,
             (unsigned long)g_dbg_ssh_loops, (unsigned long)g_dbg_term_feed,
             (unsigned long)g_dbg_render_ticks,
             (unsigned)heap_caps_get_free_size(1 << 11),
             (unsigned)heap_caps_get_free_size(1 << 10));
    float bv, bma, bmw;
    if (power_mon_get_ext(&bv, &bma, &bmw, NULL)) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        ",\"batt_v\":%.3f,\"batt_ma\":%.1f,\"batt_mw\":%.1f",
                        bv, bma, bmw);
    }
    len += snprintf(buf + len, sizeof(buf) - len,
                    ",\"screen\":\"%s\",\"idle_s\":%lu",
                    power_mgmt_screen_on() ? "on" : (power_mgmt_locked() ? "locked" : "off"),
                    (unsigned long)power_mgmt_idle_s());

    // Session tabs: observable state machine for remote verification.
    static const char *state_names[] = { "idle", "connecting", "connected" };
    len += snprintf(buf + len, sizeof(buf) - len, ",\"active\":%d,\"sessions\":[",
                    ssh_active());
    bool first = true;
    for (int id = 0; id < MAX_SSH_SESSIONS; id++) {
        char name[48];
        int tgt;
        ssh_state_t st = ssh_state(id, name, sizeof(name), &tgt);
        if (tgt < 0) continue;
        len += snprintf(buf + len, sizeof(buf) - len,
                        "%s{\"id\":%d,\"target\":%d,\"name\":\"%s\",\"state\":\"%s\"}",
                        first ? "" : ",", id, tgt, name, state_names[st]);
        first = false;
    }
    len += snprintf(buf + len, sizeof(buf) - len, "]");
    snprintf(buf + len, sizeof(buf) - len, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// POST /api/sdput?name=<file> — write request body to /sdcard/tab5/<file>.
// Formats the card on first mount if it has no filesystem
// (CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL). Used to push the asset pack from the
// host without a card reader.
static esp_err_t sdput_post(httpd_req_t *req)
{
    char query[80] = {0}, name[40] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !name[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        }
    }

    if (bsp_sdcard_mount() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd mount failed");
    }
    mkdir("/sdcard/tab5", 0775);

    char path[80];
    snprintf(path, sizeof(path), "/sdcard/tab5/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        bsp_sdcard_unmount();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }

    const size_t CHUNK = 16 * 1024;
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t remaining = req->content_len, written = 0;
    bool ok = (buf != NULL);
    while (ok && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < CHUNK ? remaining : CHUNK);
        if (r <= 0) { ok = false; break; }
        if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        remaining -= r;
        written += r;
    }
    free(buf);
    fclose(f);
    bsp_sdcard_unmount();

    if (!ok) {
        ESP_LOGE(TAG, "sdput %s failed at %u bytes", name, (unsigned)written);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    ESP_LOGI(TAG, "sdput %s: %u bytes", name, (unsigned)written);
    char msg[64];
    snprintf(msg, sizeof(msg), "wrote %u bytes", (unsigned)written);
    return httpd_resp_sendstr(req, msg);
}

// GET /api/ime?py=nihao — pinyin IME debug: run a throwaway engine query
// and return the first candidates. Remote verification without a keyboard.
static esp_err_t ime_get(httpd_req_t *req)
{
    char query[80] = {0}, py[40] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "py", py, sizeof(py)) != ESP_OK || !py[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing py");
    }

    static char cands[10][64];      // httpd task only; serialized by httpd
    int n = ime_filter_debug_query(py, cands, 10);
    if (n < 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "ime unavailable");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "pinyin", py);
    cJSON *arr = cJSON_AddArrayToObject(root, "cands");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(cands[i]));
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

// DEV-ONLY (temporary, pending the deferred HTTP-control design):
// POST /api/rec?ms=3000 — record N ms from the ES7210 mics and return the
// 16k/16-bit/mono WAV. Lets the mic chain be verified remotely without
// touching the device. Remove once voice input is trusted.
static esp_err_t rec_post(httpd_req_t *req)
{
    char query[32] = {0}, val[12] = {0};
    int ms = 3000;
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "ms", val, sizeof(val)) == ESP_OK) {
        ms = atoi(val);
    }
    if (ms < 100) ms = 100;
    if (ms > 15000) ms = 15000;

    uint8_t *wav = NULL;
    size_t len = 0;
    if (!voice_input_record_wav(ms, &wav, &len)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "mic unavailable or busy");
    }
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t err = httpd_resp_send(req, (const char *)wav, len);
    free(wav);
    return err;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// Stream an RGB565 pixel buffer as a 24-bit BMP.
static esp_err_t send_bmp_rgb565(httpd_req_t *req, const uint8_t *fb,
                                 int w, int h, int stride)
{
    int rowbytes = w * 3;                      // 24bpp rows; w*3 keeps 4-byte align for our sizes
    uint32_t imgsize = rowbytes * h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t filesize = 54 + imgsize;
    memcpy(hdr + 2, &filesize, 4);
    uint32_t off = 54;            memcpy(hdr + 10, &off, 4);
    uint32_t bisize = 40;         memcpy(hdr + 14, &bisize, 4);
    int32_t  bw = w;              memcpy(hdr + 18, &bw, 4);
    int32_t  bh = -h;             memcpy(hdr + 22, &bh, 4);   // negative = top-down
    uint16_t planes = 1;          memcpy(hdr + 26, &planes, 2);
    uint16_t bpp = 24;            memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgsize, 4);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr));

    uint8_t *row = malloc(rowbytes);
    if (!row) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    for (int y = 0; y < h; y++) {
        const uint16_t *src = (const uint16_t *)(fb + y * stride);
        for (int x = 0; x < w; x++) {
            uint16_t p = src[x];
            row[x * 3 + 0] = (p & 0x1f) << 3;          // B
            row[x * 3 + 1] = ((p >> 5) & 0x3f) << 2;   // G
            row[x * 3 + 2] = ((p >> 11) & 0x1f) << 3;  // R
        }
        if (httpd_resp_send_chunk(req, (const char *)row, rowbytes) != ESP_OK) break;
    }
    free(row);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// GET /shot — terminal canvas as a 24-bit BMP (debugging the renderer
// remotely; tearing is possible since the canvas keeps refreshing).
//
// GET /shot?full=1 — whole LVGL screen via lv_snapshot (status bar, home
// panel, ...). Top-layer overlays (sleep overlay) are not captured:
// lv_snapshot_take renders one object tree and the panel lives on the
// active screen, which is what we need to review.
//
// UI-DEV HELPER: &panel=1 / &panel=0 force the home panel open/closed
// before the snapshot — Ctrl+Alt+P can't be pressed remotely.
static esp_err_t shot_get(httpd_req_t *req)
{
    char query[64] = {0}, val[8] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    if (httpd_query_key_value(query, "panel", val, sizeof(val)) == ESP_OK) {
        ui_home_set_open(val[0] == '1');
        vTaskDelay(pdMS_TO_TICKS(300));        // let the LVGL task lay it out
    }

    // UI-DEV HELPER: &lang=0|1 forces the UI language (0 = 中文, 1 = English)
    // and live-rebuilds the panel before the snapshot. Snapshot-only; does not
    // persist to settings.lang.
    if (httpd_query_key_value(query, "lang", val, sizeof(val)) == ESP_OK) {
        ui_home_set_lang_dev(val[0] - '0');
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // UI-DEV HELPER: &edit=1 opens the SSH target editor list, &edit=2 the
    // blank add-target form (opens the panel itself if needed), &edit=0
    // back to the card grid.
    if (httpd_query_key_value(query, "edit", val, sizeof(val)) == ESP_OK) {
        ui_home_set_edit_view(val[0] - '0');
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // UI-DEV HELPER: &sheet=<n> opens the device action sheet for target n.
    if (httpd_query_key_value(query, "sheet", val, sizeof(val)) == ESP_OK) {
        ui_home_show_sheet(val[0] - '0');
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // UI-DEV HELPER: &keys=1 opens the SSH key panel (over the expanded SSH
    // app), &keys=0 backs out to the SSH app.
    if (httpd_query_key_value(query, "keys", val, sizeof(val)) == ESP_OK) {
        ui_home_set_keys_view(val[0] - '0');
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // UI-DEV HELPER: &conn=1 expanded 连接 app, &conn=2 添加网络 form, &conn=0
    // back to the card grid.
    if (httpd_query_key_value(query, "conn", val, sizeof(val)) == ESP_OK) {
        ui_home_set_conn_view(val[0] - '0');
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    bool full = httpd_query_key_value(query, "full", val, sizeof(val)) == ESP_OK
                && val[0] == '1';
    if (!full) {
        int w, h, stride;
        const uint8_t *fb = term_render_framebuffer(&w, &h, &stride);
        if (!fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no fb");
        return send_bmp_rgb565(req, fb, w, h, stride);
    }

    bsp_display_lock(0);
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();
    if (!snap) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");

    esp_err_t err = send_bmp_rgb565(req, snap->data, snap->header.w,
                                    snap->header.h, snap->header.stride);
    bsp_display_lock(0);
    lv_draw_buf_destroy(snap);
    bsp_display_unlock();
    return err;
}

// POST /api/connect {"index":n} — open-or-switch to a session on target n.
// With session tabs this opens a NEW session (or switches to an existing one
// for the same target) instead of retargeting the active session: the web UI
// button reads "connect", and dropping someone's live session to do it would
// be the more surprising behavior.
static esp_err_t connect_post(httpd_req_t *req)
{
    char body[64];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    cJSON *root = cJSON_Parse(body);
    cJSON *idx = root ? cJSON_GetObjectItem(root, "index") : NULL;
    if (idx && cJSON_IsNumber(idx)) {
        ssh_client_request_switch(idx->valueint);
    }
    if (root) cJSON_Delete(root);
    return httpd_resp_sendstr(req, "ok");
}

void web_config_start(settings_t *s, void (*on_update)(void))
{
    s_cfg = s;
    s_on_update = on_update;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;     // /shot?full=1 renders LVGL on this stack
    cfg.max_uri_handlers = 16;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/targets", .method = HTTP_GET,  .handler = targets_get },
        { .uri = "/api/targets", .method = HTTP_POST, .handler = targets_post },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/shot",        .method = HTTP_GET,  .handler = shot_get },
        { .uri = "/debug",       .method = HTTP_GET,  .handler = debug_get },
        { .uri = "/api/ime",     .method = HTTP_GET,  .handler = ime_get },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/sdput",   .method = HTTP_POST, .handler = sdput_post },
        { .uri = "/api/rec",     .method = HTTP_POST, .handler = rec_post },   // DEV-ONLY mic check
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "web config up on port 80");
}
