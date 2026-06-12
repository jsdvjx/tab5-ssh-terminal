// SSH session task built on libssh2 (skuodi/libssh2_esp, mbedTLS backend).
// One task owns the socket + session; input from other tasks arrives through
// a stream buffer so libssh2 (not thread-safe per session) stays single-task.
// Supports multiple targets with runtime switching (panel tap / web config).

#include "ssh_client.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "libssh2.h"
#include "ui_panel.h"

static const char *TAG = "ssh";

#define TX_BUF_SIZE 1024
#define RX_CHUNK    2048

static term_t *s_term;
static settings_t *s_cfg;
static ssh_target_changed_cb_t s_changed_cb;
static StreamBufferHandle_t s_tx;
static volatile bool s_connected;
static volatile int s_pending_switch = -1;
static volatile bool s_pending_resize;
volatile uint32_t g_dbg_ssh_rx, g_dbg_ssh_tx, g_dbg_ssh_loops;

void ssh_client_send(const uint8_t *data, size_t len)
{
    if (!s_connected || !s_tx) return;
    xStreamBufferSend(s_tx, data, len, pdMS_TO_TICKS(100));
}

void ssh_client_request_switch(int index)
{
    if (!s_cfg || index < 0 || index >= s_cfg->n_targets) return;
    s_pending_switch = index;
}

void ssh_client_request_resize(void)
{
    s_pending_resize = true;
}

static void term_response_cb(const uint8_t *data, size_t len, void *ctx)
{
    ssh_client_send(data, len);
}

static void feed_status(const char *msg)
{
    term_feed(s_term, (const uint8_t *)msg, strlen(msg));
}

static int open_socket(const ssh_target_t *t)
{
    char port[8];
    snprintf(port, sizeof(port), "%d", t->port);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    if (getaddrinfo(t->host, port, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", t->host);
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock >= 0 && connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

// One full connect->shell->disconnect cycle. Returns when the session drops
// or a switch is requested.
static void session_run(const ssh_target_t *t)
{
    int sock = open_socket(t);
    if (sock < 0) {
        feed_status("\r\n[tab5] connect failed\r\n");
        ui_panel_set_state("connect failed");
        return;
    }

    LIBSSH2_SESSION *session = libssh2_session_init();
    libssh2_session_set_blocking(session, 1);
    if (libssh2_session_handshake(session, sock) != 0) {
        feed_status("\r\n[tab5] SSH handshake failed\r\n");
        ui_panel_set_state("handshake failed");
        goto out_sock;
    }

    // TODO: verify host key fingerprint against one pinned in NVS
    // (libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256))

    if (libssh2_userauth_password(session, t->user, t->pass) != 0) {
        feed_status("\r\n[tab5] auth failed\r\n");
        ui_panel_set_state("auth failed");
        goto out_session;
    }

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel) goto out_session;

    libssh2_channel_request_pty_ex(channel, "xterm-256color", 14,
                                   NULL, 0, term_cols(s_term), TERM_ROWS, 0, 0);

    if (strlen(t->cmd) > 0) {
        if (libssh2_channel_exec(channel, t->cmd) != 0) goto out_channel;
    } else {
        if (libssh2_channel_shell(channel) != 0) goto out_channel;
    }

    ESP_LOGI(TAG, "shell open: %s@%s", t->user, t->host);
    ui_panel_set_state("connected");
    s_connected = true;
    xStreamBufferReset(s_tx);
    libssh2_session_set_blocking(session, 0);

    uint8_t buf[RX_CHUNK];
    while (!libssh2_channel_eof(channel)) {
        if (s_pending_switch >= 0) break;
        if (s_pending_resize) {
            s_pending_resize = false;
            libssh2_channel_request_pty_size(channel, term_cols(s_term), TERM_ROWS);
        }
        bool idle = true;

        g_dbg_ssh_loops++;
        ssize_t n = libssh2_channel_read(channel, (char *)buf, sizeof(buf));
        if (n > 0) {
            g_dbg_ssh_rx += n;
            term_feed(s_term, buf, n);
            idle = false;
        } else if (n != LIBSSH2_ERROR_EAGAIN && n < 0) {
            break;
        }

        size_t txn = xStreamBufferReceive(s_tx, buf, sizeof(buf), 0);
        if (txn > 0) {
            size_t off = 0;
            while (off < txn) {
                ssize_t w = libssh2_channel_write(channel, (char *)buf + off, txn - off);
                if (w == LIBSSH2_ERROR_EAGAIN) { vTaskDelay(1); continue; }
                if (w < 0) goto drop;
                g_dbg_ssh_tx += w;
                off += w;
            }
            idle = false;
        }

        if (idle) vTaskDelay(pdMS_TO_TICKS(5));
    }
drop:
    ESP_LOGW(TAG, "channel closed");

out_channel:
    s_connected = false;
    libssh2_session_set_blocking(session, 1);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
out_session:
    libssh2_session_disconnect(session, "bye");
    libssh2_session_free(session);
out_sock:
    close(sock);
}

static void ssh_task(void *arg)
{
    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init failed: %d", rc);
        vTaskDelete(NULL);
    }

    int backoff_s = 1;
    while (true) {
        if (s_cfg->n_targets == 0) {
            feed_status("\r\n[tab5] no SSH targets - add one via the web config\r\n");
            ui_panel_set_state("no targets");
            while (s_cfg->n_targets == 0 && s_pending_switch < 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        if (s_pending_switch >= 0) {
            s_cfg->last_target = s_pending_switch;
            s_pending_switch = -1;
            backoff_s = 1;
            if (s_changed_cb) s_changed_cb(s_cfg->last_target);
        }
        if (s_cfg->last_target >= s_cfg->n_targets) s_cfg->last_target = 0;
        const ssh_target_t *t = &s_cfg->targets[s_cfg->last_target];

        char banner[160];
        snprintf(banner, sizeof(banner), "\r\n[tab5] connecting to %s@%s:%d ...\r\n",
                 t->user, t->host, t->port);
        feed_status(banner);
        ui_panel_set_state("connecting...");

        session_run(t);

        if (s_pending_switch >= 0) continue;   // switch immediately, no backoff

        feed_status("\r\n[tab5] disconnected, retrying...\r\n");
        vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
        if (backoff_s < 30) backoff_s *= 2;
    }
}

void ssh_client_start(term_t *t, settings_t *s, ssh_target_changed_cb_t cb)
{
    s_term = t;
    s_cfg = s;
    s_changed_cb = cb;
    s_tx = xStreamBufferCreate(TX_BUF_SIZE, 1);
    term_set_response_cb(t, term_response_cb, NULL);
    // 16KB stack: mbedTLS key exchange is stack-hungry
    xTaskCreatePinnedToCore(ssh_task, "ssh", 16384, NULL, 5, NULL, 1);
}
