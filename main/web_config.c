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

#include "ssh_client.h"
#include "term_render.h"

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
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"kbd_events\":%lu,\"sink_bytes\":%lu,\"ssh_rx\":%lu,"
             "\"ssh_tx\":%lu,\"ssh_loops\":%lu,\"term_feed\":%lu,"
             "\"render_ticks\":%lu,\"free_int\":%u,\"free_psram\":%u}",
             (unsigned long)g_dbg_kbd_events, (unsigned long)g_dbg_sink_bytes,
             (unsigned long)g_dbg_ssh_rx, (unsigned long)g_dbg_ssh_tx,
             (unsigned long)g_dbg_ssh_loops, (unsigned long)g_dbg_term_feed,
             (unsigned long)g_dbg_render_ticks,
             (unsigned)heap_caps_get_free_size(1 << 11),
             (unsigned)heap_caps_get_free_size(1 << 10));
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

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// GET /shot — terminal canvas as a 24-bit BMP (debugging the renderer
// remotely; tearing is possible since the canvas keeps refreshing).
static esp_err_t shot_get(httpd_req_t *req)
{
    int w, h, stride;
    const uint8_t *fb = term_render_framebuffer(&w, &h, &stride);
    if (!fb) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no fb");

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
    cfg.stack_size = 8192;
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
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/sdput",   .method = HTTP_POST, .handler = sdput_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "web config up on port 80");
}
