// T5NAT/1 reverse-tunnel client. See docs/nat-protocol.md.
//
// Design (RAM-tight device — see the bring-up gotchas note):
//   * ONE esp_websocket_client transport (its own internal task) carries the
//     single WSS connection. Inbound WS BINARY messages are framed in the
//     event callback (ws_event) and dispatched: control frames acted on
//     directly, OPEN/DATA/CLOSE handed to the stream table.
//   * ONE "pump" task select()s over all active local sockets and forwards
//     readable bytes to the relay as DATA frames. A self-pipe wakes it when a
//     new stream opens / one closes / a stop is requested, so select() never
//     blocks longer than the keepalive cadence. This is far cheaper than a
//     task-per-stream (8 x 4KB = 32KB internal RAM); the whole client is one
//     extra ~5KB task.
//   * Streams: a fixed array of MAX_STREAMS {id, fd, used}. The pump task and
//     the ws_event callback both touch it, so a mutex guards the table.
//   * Reconnect: the pump task owns the connect/backoff loop. On any drop it
//     tears down streams and reconnects with exponential backoff (2..30s).
//
// NVS/flash note: the pump task calls settings only via the cached pointer
// (no NVS writes), and esp_websocket_client's TLS lives in PSRAM-friendly
// heap; the pump task stack is internal RAM (default xTaskCreate) so esp-tls /
// lwip flash-resident code is safe to run on it.

#include "nat_tunnel.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_eventfd.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "nat";

// ---- protocol constants (mirror docs/nat-protocol.md) ----------------------
enum {
    T_HELLO = 0x01,
    T_READY = 0x02,
    T_OPEN  = 0x10,
    T_DATA  = 0x11,
    T_CLOSE = 0x12,
    T_PING  = 0x20,
    T_PONG  = 0x21,
};

#define MAX_STREAMS          8
#define NAT_DATA_CHUNK       1460     // max bytes per outgoing DATA frame
#define NAT_PING_INTERVAL_S  25
#define NAT_PING_TIMEOUT_S   60
#define NAT_RX_FRAME_MAX     8192     // largest inbound WS payload we accept
#define NAT_HDR              3        // [u8 type][u16 id]

// ---- stream table ----------------------------------------------------------
typedef struct {
    bool     used;
    uint16_t id;       // relay-assigned stream id
    int      fd;       // local TCP socket to 127.0.0.1:port
} stream_t;

static stream_t        s_streams[MAX_STREAMS];
static SemaphoreHandle_t s_lock;       // guards s_streams + s_host + s_connected

// ---- module state ----------------------------------------------------------
static settings_t            *s_cfg;
static esp_websocket_client_handle_t s_ws;
static TaskHandle_t           s_task;
static volatile bool          s_run;        // start() sets, stop() clears
static volatile bool          s_connected;  // READY ok received
static volatile bool          s_ws_open;    // WS transport connected
static volatile bool          s_want_reconnect;  // drop detected -> recycle
static char                   s_host[64];   // assigned public host
static int                    s_wake_fd = -1;  // eventfd to wake select() on new OPEN/stop
static volatile int64_t       s_last_rx_us;  // last inbound frame (for ping timeout)
static volatile int64_t       s_last_ping_us;

// Reassembly for fragmented WS messages (esp_websocket_client may split a
// large message across several WEBSOCKET_EVENT_DATA callbacks).
static uint8_t  *s_rx;            // PSRAM buffer, NAT_RX_FRAME_MAX
static int       s_rx_len;        // bytes accumulated for the current message
static int       s_rx_total;      // total expected (payload_len) for the message

// --------------------------------------------------------------------------
static int64_t now_us(void) { return esp_timer_get_time(); }

static void wake_pump(void)
{
    if (s_wake_fd >= 0) {
        uint64_t one = 1;
        (void)write(s_wake_fd, &one, sizeof(one));
    }
}

// ---- frame send helpers ----------------------------------------------------
// All app frames are single WS BINARY messages: [type][id_be][payload].
static int ws_send_frame(uint8_t type, uint16_t id, const void *payload, int plen)
{
    if (!s_ws || !s_ws_open) return -1;
    // send_bin sends one WS message per call, so header+payload must be
    // contiguous. Small frames build on the stack; larger ones use the heap.
    int total = NAT_HDR + (plen > 0 ? plen : 0);
    uint8_t stackbuf[NAT_HDR + 64];
    uint8_t *buf = (total <= (int)sizeof(stackbuf))
                   ? stackbuf
                   : malloc(total);
    if (!buf) return -1;
    buf[0] = type;
    buf[1] = (uint8_t)(id >> 8);
    buf[2] = (uint8_t)(id & 0xff);
    if (plen > 0 && payload) memcpy(buf + NAT_HDR, payload, plen);
    int w = esp_websocket_client_send_bin(s_ws, (const char *)buf, total,
                                          pdMS_TO_TICKS(5000));
    if (buf != stackbuf) free(buf);
    return w;
}

static void send_close(uint16_t id, const char *reason)
{
    ws_send_frame(T_CLOSE, id, reason, reason ? (int)strlen(reason) : 0);
}

// ---- stream table ops (call under s_lock unless noted) ---------------------
static stream_t *stream_find(uint16_t id)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (s_streams[i].used && s_streams[i].id == id) return &s_streams[i];
    return NULL;
}

static stream_t *stream_alloc(uint16_t id)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!s_streams[i].used) {
            s_streams[i].used = true;
            s_streams[i].id = id;
            s_streams[i].fd = -1;
            return &s_streams[i];
        }
    return NULL;
}

static void stream_free_locked(stream_t *st)
{
    if (!st || !st->used) return;
    if (st->fd >= 0) { close(st->fd); st->fd = -1; }
    st->used = false;
    st->id = 0;
}

static void streams_close_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_STREAMS; i++) stream_free_locked(&s_streams[i]);
    xSemaphoreGive(s_lock);
}

static bool port_allowed(uint16_t port)
{
    // The exposed-ports list isn't carried in settings yet; the contract is
    // [80, 8080] (web_config + a spare). Keep this in sync with HELLO below.
    return port == 80 || port == 8080;
}

// ---- OPEN handling: connect a local socket to 127.0.0.1:port ---------------
static void handle_open(uint16_t id, uint16_t port)
{
    if (!port_allowed(port)) {
        ESP_LOGW(TAG, "OPEN id=%u port=%u not allowed", id, port);
        send_close(id, "port not exposed");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (stream_find(id)) { xSemaphoreGive(s_lock); return; }   // dup
    stream_t *st = stream_alloc(id);
    xSemaphoreGive(s_lock);
    if (!st) {
        ESP_LOGW(TAG, "OPEN id=%u: no free stream slot", id);
        send_close(id, "too many streams");
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { send_close(id, "socket"); goto fail; }
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGW(TAG, "OPEN id=%u: connect 127.0.0.1:%u failed (%d)", id, port, errno);
        close(fd);
        send_close(id, "connect failed");
        goto fail;
    }
    // Non-blocking so the pump's select() loop never stalls on a slow local read.
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->fd = fd;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "stream %u -> 127.0.0.1:%u open (fd=%d)", id, port, fd);
    wake_pump();    // let the pump add this fd to its select set
    return;

fail:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream_free_locked(st);
    xSemaphoreGive(s_lock);
}

// ---- DATA from relay: write to the stream's local socket -------------------
static void handle_data(uint16_t id, const uint8_t *p, int len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream_t *st = stream_find(id);
    int fd = (st && st->fd >= 0) ? st->fd : -1;
    xSemaphoreGive(s_lock);
    if (fd < 0) { send_close(id, "no stream"); return; }

    // Blocking-ish write loop; the fd is non-blocking, so spin briefly on
    // EAGAIN (the local web server drains fast). Drop+close on hard error.
    int off = 0;
    int spins = 0;
    while (off < len) {
        int w = send(fd, p + off, len - off, 0);
        if (w > 0) { off += w; spins = 0; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++spins > 200) break;        // ~200ms; visitor stalled
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        // EOF/error: tear the stream down.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        stream_free_locked(stream_find(id));
        xSemaphoreGive(s_lock);
        send_close(id, "local write");
        wake_pump();
        return;
    }
}

static void handle_close(uint16_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stream_t *st = stream_find(id);
    bool had = (st != NULL);
    stream_free_locked(st);
    xSemaphoreGive(s_lock);
    if (had) wake_pump();
}

// ---- inbound frame dispatch (called from ws_event after reassembly) --------
static void dispatch_frame(const uint8_t *f, int n)
{
    if (n < 1) return;
    uint8_t type = f[0];
    uint16_t id = (n >= NAT_HDR) ? ((uint16_t)f[1] << 8 | f[2]) : 0;
    const uint8_t *p = f + NAT_HDR;
    int plen = n - NAT_HDR;
    if (plen < 0) plen = 0;

    s_last_rx_us = now_us();

    switch (type) {
    case T_READY: {
        cJSON *root = cJSON_ParseWithLength((const char *)p, plen);
        bool ok = false;
        char host[64] = { 0 };
        if (root) {
            cJSON *jok = cJSON_GetObjectItem(root, "ok");
            ok = cJSON_IsTrue(jok);
            cJSON *jhost = cJSON_GetObjectItem(root, "host");
            if (ok && cJSON_IsString(jhost)) strlcpy(host, jhost->valuestring, sizeof(host));
            if (!ok) {
                cJSON *jerr = cJSON_GetObjectItem(root, "err");
                ESP_LOGW(TAG, "READY not ok: %s",
                         cJSON_IsString(jerr) ? jerr->valuestring : "?");
            }
            cJSON_Delete(root);
        }
        if (ok) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s_host, host, sizeof(s_host));
            s_connected = true;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "READY ok host=%s", host);
        } else {
            s_want_reconnect = true;   // pump recycles with backoff
            wake_pump();
        }
        break;
    }
    case T_OPEN: {
        if (plen < 2) { send_close(id, "bad open"); break; }
        uint16_t port = (uint16_t)p[0] << 8 | p[1];
        handle_open(id, port);
        break;
    }
    case T_DATA:
        if (id) handle_data(id, p, plen);
        break;
    case T_CLOSE:
        if (id) handle_close(id);
        break;
    case T_PING:
        ws_send_frame(T_PONG, 0, p, plen);   // echo payload
        break;
    case T_PONG:
        break;                               // liveness updated via s_last_rx_us
    default:
        ESP_LOGD(TAG, "unknown frame type 0x%02x len=%d", type, n);
        break;
    }
}

// ---- WebSocket event callback (esp_websocket_client transport task) --------
static void rx_reset(void) { s_rx_len = 0; s_rx_total = 0; }

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected");
        s_ws_open = true;
        rx_reset();
        wake_pump();        // pump sends HELLO once the WS is up
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WS closed/disconnected");
        s_ws_open = false;
        s_connected = false;
        s_want_reconnect = true;
        wake_pump();
        break;
    case WEBSOCKET_EVENT_DATA: {
        // op_code 2 = binary, 0 = continuation, 8 = close, 9/10 = ping/pong.
        if (d->op_code == 0x08) break;                 // WS close handled above
        if (d->op_code != 0x02 && d->op_code != 0x00) break;  // ignore text/ctrl
        if (d->payload_len <= 0) break;
        if (d->payload_len > NAT_RX_FRAME_MAX) {
            ESP_LOGW(TAG, "oversized WS msg %d, dropping", d->payload_len);
            rx_reset();
            break;
        }
        // First fragment of a message: payload_offset == 0.
        if (d->payload_offset == 0) { rx_reset(); s_rx_total = d->payload_len; }
        int copy = d->data_len;
        if (s_rx_len + copy > NAT_RX_FRAME_MAX) copy = NAT_RX_FRAME_MAX - s_rx_len;
        if (copy > 0 && s_rx) {
            memcpy(s_rx + s_rx_len, d->data_ptr, copy);
            s_rx_len += copy;
        }
        // Message complete when we've accumulated payload_len bytes.
        if (s_rx_len >= s_rx_total && s_rx_total > 0) {
            dispatch_frame(s_rx, s_rx_len);
            rx_reset();
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS error");
        break;
    default:
        break;
    }
}

// ---- HELLO -----------------------------------------------------------------
static void send_hello(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "token", s_cfg->nat_token);
    cJSON_AddStringToObject(root, "sub", s_cfg->nat_sub);
    cJSON *ports = cJSON_AddArrayToObject(root, "ports");
    cJSON_AddItemToArray(ports, cJSON_CreateNumber(80));
    cJSON_AddItemToArray(ports, cJSON_CreateNumber(8080));
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        ws_send_frame(T_HELLO, 0, json, (int)strlen(json));
        ESP_LOGI(TAG, "HELLO sent: %s", json);
        free(json);
    }
}

// ---- WS transport lifecycle ------------------------------------------------
static bool ws_open(void)
{
    if (!s_cfg->nat_url[0]) {
        ESP_LOGW(TAG, "nat_url empty");
        return false;
    }
    esp_websocket_client_config_t cfg = {
        .uri = s_cfg->nat_url,
        .subprotocol = "t5nat1",
        .reconnect_timeout_ms = 0,        // we own reconnect/backoff
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .buffer_size = 2048,
        .task_stack = 6144,
    };
    // wss:// uses the system cert bundle via esp-tls; ws:// (mock) is plaintext.
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "ws init failed"); return false; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "ws start failed");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return false;
    }
    return true;
}

static void ws_close(void)
{
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_open = false;
    s_connected = false;
}

// ---- pump task: connect/backoff loop + local-socket select() ---------------
static void pump_select(void)
{
    s_last_rx_us = now_us();
    s_last_ping_us = now_us();
    bool hello_sent = false;

    while (s_run && !s_want_reconnect) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = s_wake_fd;
        if (s_wake_fd >= 0) FD_SET(s_wake_fd, &rfds);

        // Send HELLO once the WS reports connected.
        if (s_ws_open && !hello_sent) { send_hello(); hello_sent = true; }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (s_streams[i].used && s_streams[i].fd >= 0) {
                FD_SET(s_streams[i].fd, &rfds);
                if (s_streams[i].fd > maxfd) maxfd = s_streams[i].fd;
            }
        }
        xSemaphoreGive(s_lock);

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0 && s_wake_fd >= 0 && FD_ISSET(s_wake_fd, &rfds)) {
            uint64_t drain;
            (void)read(s_wake_fd, &drain, sizeof(drain));   // eventfd: one read clears
        }

        // Pump readable local sockets -> DATA frames.
        if (r > 0) {
            // Snapshot the active (id,fd) pairs under the lock, then read
            // outside it to avoid holding the lock across send().
            struct { uint16_t id; int fd; } act[MAX_STREAMS];
            int nact = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < MAX_STREAMS; i++) {
                if (s_streams[i].used && s_streams[i].fd >= 0 &&
                    FD_ISSET(s_streams[i].fd, &rfds)) {
                    act[nact].id = s_streams[i].id;
                    act[nact].fd = s_streams[i].fd;
                    nact++;
                }
            }
            xSemaphoreGive(s_lock);

            static uint8_t buf[NAT_DATA_CHUNK];
            for (int i = 0; i < nact; i++) {
                int n = recv(act[i].fd, buf, sizeof(buf), 0);
                if (n > 0) {
                    ws_send_frame(T_DATA, act[i].id, buf, n);
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // EOF or error: close + CLOSE frame, free the slot.
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    stream_free_locked(stream_find(act[i].id));
                    xSemaphoreGive(s_lock);
                    send_close(act[i].id, "local eof");
                }
            }
        }

        // App-level keepalive + dead-peer detection.
        int64_t t = now_us();
        if (s_connected) {
            if (t - s_last_ping_us > (int64_t)NAT_PING_INTERVAL_S * 1000000) {
                ws_send_frame(T_PING, 0, NULL, 0);
                s_last_ping_us = t;
            }
            if (t - s_last_rx_us > (int64_t)NAT_PING_TIMEOUT_S * 1000000) {
                ESP_LOGW(TAG, "ping timeout, reconnecting");
                s_want_reconnect = true;
            }
        }
    }
}

static void pump_task(void *arg)
{
    int backoff_s = 2;
    for (;;) {
        // Parked between stop() and the next start(): a wake_pump() write to the
        // self-pipe (or just the poll) unblocks us once s_run is set again.
        if (!s_run) {
            backoff_s = 2;
            ESP_LOGI(TAG, "pump idle (stopped)");
            while (!s_run) vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGI(TAG, "pump resumed");
        }

        s_want_reconnect = false;
        if (!ws_open()) {
            for (int i = 0; i < backoff_s * 10 && s_run; i++) vTaskDelay(pdMS_TO_TICKS(100));
            if (backoff_s < 30) backoff_s *= 2;
            continue;
        }
        ESP_LOGI(TAG, "dialing %s", s_cfg->nat_url);

        pump_select();   // runs until s_want_reconnect or !s_run

        bool was_connected = s_connected;
        ws_close();
        streams_close_all();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_host[0] = 0;
        xSemaphoreGive(s_lock);

        if (!s_run) continue;           // stop(): loop back to the park above
        // Reset backoff after a session that actually reached READY.
        if (was_connected) backoff_s = 2;
        ESP_LOGW(TAG, "reconnect in %ds", backoff_s);
        for (int i = 0; i < backoff_s * 10 && s_run; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (backoff_s < 30) backoff_s *= 2;
    }
}

// ---- public API ------------------------------------------------------------
void nat_tunnel_init(settings_t *s)
{
    s_cfg = s;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_rx) s_rx = heap_caps_malloc(NAT_RX_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx) s_rx = malloc(NAT_RX_FRAME_MAX);   // fall back to internal
}

void nat_tunnel_start(void)
{
    if (!s_cfg || !s_lock || !s_rx) {
        ESP_LOGE(TAG, "start before init");
        return;
    }
    if (s_run) return;
    if (s_wake_fd < 0) {
        // eventfd wakes the pump's select() on a new OPEN / stop (pipe() is not
        // supported in IDF). Registration is idempotent across restarts.
        static bool evt_reg;
        if (!evt_reg) {
            esp_vfs_eventfd_config_t ec = { .max_fds = 1 };
            esp_err_t er = esp_vfs_eventfd_register(&ec);
            evt_reg = (er == ESP_OK || er == ESP_ERR_INVALID_STATE);  // already-reg ok
        }
        s_wake_fd = eventfd(0, 0);
        if (s_wake_fd < 0) {
            ESP_LOGW(TAG, "eventfd failed (%d); pump wakes on 1s timeout only", errno);
        }
    }
    s_run = true;
    if (!s_task) {
        // Internal-RAM stack (default): the task runs esp-tls / lwip flash code.
        xTaskCreate(pump_task, "nat", 6144, NULL, 4, &s_task);
    } else {
        wake_pump();   // re-arm a parked task
    }
    ESP_LOGI(TAG, "started");
}

void nat_tunnel_stop(void)
{
    if (!s_run) return;
    s_run = false;
    s_want_reconnect = true;
    wake_pump();
    ESP_LOGI(TAG, "stop requested");
}

bool nat_tunnel_status(char *host, size_t cap)
{
    // May be called from the UI before nat_tunnel_init() (panel refresh runs
    // early); the lock isn't created yet then.
    if (!s_lock) {
        if (host && cap) host[0] = 0;
        return false;
    }
    bool conn;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    conn = s_connected;
    if (host && cap) {
        if (conn) strlcpy(host, s_host, cap);
        else host[0] = 0;
    }
    xSemaphoreGive(s_lock);
    return conn;
}
