// Concurrent SSH sessions ("tabs") built on libssh2 (skuodi/libssh2_esp,
// mbedTLS backend). One task per session owns that session's socket +
// libssh2 state; input from other tasks arrives through a per-session stream
// buffer so libssh2 (not thread-safe per session) stays single-task.
//
// Only the active session's term_t is rendered (term_render_set_term);
// background sessions keep feeding their own grids, which is cheap.
// Task stacks live in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY):
// the session tasks never touch flash, and 16KB x 4 of internal stack would
// eat most of what's left of internal RAM.

#include "ssh_client.h"
#include "local_shell.h"

#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "libssh2.h"
#include "term_render.h"
#include "ssh_keys.h"

static const char *TAG = "ssh";

#define TX_BUF_SIZE 1024
#define RX_CHUNK    2048
#define SSH_STACK   16384           // mbedTLS key exchange is stack-hungry
#define MIN_FREE_INTERNAL (40 * 1024)   // refuse to open a session below this

typedef struct {
    bool used;                      // slot allocated (task alive or starting)
    volatile bool closing;          // close requested; hidden from UI/active
    int target;                     // index into s_cfg->targets
    term_t *term;                   // owned grid (slot terms are cached)
    StreamBufferHandle_t tx;
    volatile bool connected;
    volatile bool pending_resize;
    volatile ssh_state_t state;
    char target_name[48];
} ssh_session_t;

static settings_t *s_cfg;
static ssh_target_changed_cb_t s_changed_cb;
static term_t *s_term0;             // boot terminal == session slot 0's term
static ssh_session_t s_sessions[MAX_SSH_SESSIONS];
static term_t *s_slot_term[MAX_SSH_SESSIONS];   // term_t has no destroy: cache
static volatile int s_active = -1;
static SemaphoreHandle_t s_lock;    // guards slot alloc/free + active switch
volatile uint32_t g_dbg_ssh_rx, g_dbg_ssh_tx, g_dbg_ssh_loops;

// ---------------------------------------------------------------- helpers

static bool valid_id(int id)
{
    return id >= 0 && id < MAX_SSH_SESSIONS;
}

static void feed_str(term_t *t, const char *msg)
{
    term_feed(t, (const uint8_t *)msg, strlen(msg));
}

// Status/refusal messages go to whatever the user is looking at.
static void feed_active(const char *msg)
{
    term_t *t = ssh_active_term();
    if (t) feed_str(t, msg);
}

static void term_response_cb(const uint8_t *data, size_t len, void *ctx)
{
    ssh_send((int)(intptr_t)ctx, data, len);
}

// ---------------------------------------------------------------- queries

term_t *ssh_active_term(void)
{
    int id = s_active;
    if (valid_id(id) && s_sessions[id].used) return s_sessions[id].term;
    return s_term0;
}

int ssh_active(void)
{
    int id = s_active;
    return (valid_id(id) && s_sessions[id].used && !s_sessions[id].closing) ? id : -1;
}

int ssh_session_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SSH_SESSIONS; i++) {
        if (s_sessions[i].used && !s_sessions[i].closing) n++;
    }
    return n;
}

ssh_state_t ssh_state(int id, char *target, size_t cap, int *target_idx)
{
    if (!valid_id(id) || !s_sessions[id].used || s_sessions[id].closing) {
        if (target && cap) target[0] = 0;
        if (target_idx) *target_idx = -1;
        return SSH_STATE_IDLE;
    }
    if (target && cap) strlcpy(target, s_sessions[id].target_name, cap);
    if (target_idx) *target_idx = s_sessions[id].target;
    return s_sessions[id].state;
}

ssh_state_t ssh_client_get_state(char *target, size_t cap)
{
    return ssh_state(ssh_active(), target, cap, NULL);
}

// ---------------------------------------------------------------- I/O

void ssh_send(int id, const uint8_t *data, size_t len)
{
    if (!valid_id(id)) return;
    ssh_session_t *ss = &s_sessions[id];
    if (!ss->used || !ss->connected || !ss->tx) return;
    xStreamBufferSend(ss->tx, data, len, pdMS_TO_TICKS(100));
}

void ssh_client_send(const uint8_t *data, size_t len)
{
    ssh_send(ssh_active(), data, len);
}

void ssh_client_request_resize(void)
{
    for (int i = 0; i < MAX_SSH_SESSIONS; i++) {
        if (s_sessions[i].used) s_sessions[i].pending_resize = true;
    }
}

// ---------------------------------------------------------------- switching

bool ssh_set_active(int id)
{
    if (!valid_id(id) || !s_sessions[id].used || s_sessions[id].closing) return false;
    if (s_active == id) return true;
    s_active = id;
    local_shell_leave();                          // keyboard -> this session
    term_render_set_term(s_sessions[id].term);   // full redraw inside
    if (s_cfg && s_sessions[id].target < s_cfg->n_targets) {
        s_cfg->last_target = s_sessions[id].target;
        if (s_changed_cb) s_changed_cb(s_cfg->last_target);
    }
    return true;
}

// ---------------------------------------------------------------- session task

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

// One full connect->shell->disconnect cycle for one session. Returns when
// the connection drops or a close is requested.
static void session_run(ssh_session_t *ss, const ssh_target_t *t)
{
    int sock = open_socket(t);
    if (sock < 0) {
        feed_str(ss->term, "\r\n[tab5] connect failed\r\n");
        return;
    }

    LIBSSH2_SESSION *session = libssh2_session_init();
    libssh2_session_set_blocking(session, 1);
    if (libssh2_session_handshake(session, sock) != 0) {
        feed_str(ss->term, "\r\n[tab5] SSH handshake failed\r\n");
        goto out_sock;
    }

    // Host key pinning: SHA256 fingerprint per host:port in NVS. First
    // sight pins; any later mismatch aborts hard (no auto-retry into a
    // possibly MITM'd host — backoff in the caller will retry the network,
    // but every attempt re-fails here until `hostkey clear <n>`).
    const char *hk = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hk) {
        feed_str(ss->term, "\r\n[tab5] no host key fingerprint - aborting\r\n");
        goto out_session;
    }
    uint8_t old_fp[32];
    char fp64[48], msg[256];
    hostkey_result_t hkr = ssh_hostkey_check(t->host, t->port,
                                             (const uint8_t *)hk, old_fp);
    if (hkr == HOSTKEY_NEW) {
        ssh_fp_base64((const uint8_t *)hk, fp64, sizeof(fp64));
        snprintf(msg, sizeof(msg), "\r\n[tab5] pinned host key SHA256:%s\r\n", fp64);
        feed_str(ss->term, msg);
    } else if (hkr == HOSTKEY_MISMATCH) {
        char old64[48];
        ssh_fp_base64((const uint8_t *)hk, fp64, sizeof(fp64));
        ssh_fp_base64(old_fp, old64, sizeof(old64));
        feed_str(ss->term,
                 "\r\n\x1b[1;37;41m @@@ WARNING: REMOTE HOST KEY CHANGED @@@ \x1b[0m\r\n"
                 "\x1b[1;31mSomeone could be eavesdropping (man-in-the-middle),"
                 "\r\nor the host key was simply regenerated.\x1b[0m\r\n");
        snprintf(msg, sizeof(msg),
                 "pinned:  SHA256:%s\r\npresent: SHA256:%s\r\n"
                 "If this change is expected, run \x1b[1mhostkey clear %d\x1b[0m"
                 " in the local shell (Ctrl+Alt+T), then reconnect.\r\n",
                 old64, fp64, ss->target);
        feed_str(ss->term, msg);
        goto out_session;
    } else if (hkr == HOSTKEY_ERROR) {
        feed_str(ss->term, "\r\n[tab5] host key pin store error - aborting\r\n");
        goto out_session;
    }

    // Auth method per target (settings_t.target_auth[]):
    //   AUTO     = device key first, then password (default / old behaviour)
    //   PASSWORD = password only
    //   KEY      = device key (证书) only
    const char *auth_used = NULL;
    const char *pem = ssh_keys_private_pem();
    int mode = (ss->target >= 0 && ss->target < MAX_SSH_TARGETS)
                   ? s_cfg->target_auth[ss->target] : TARGET_AUTH_AUTO;
    bool try_key = (mode != TARGET_AUTH_PASSWORD) && pem;
    bool try_pass = (mode != TARGET_AUTH_KEY);

    if (try_key && libssh2_userauth_publickey_frommemory(session,
                t->user, strlen(t->user), NULL, 0,
                pem, strlen(pem), NULL) == 0) {
        auth_used = "publickey";
    } else if (try_pass && libssh2_userauth_password(session, t->user, t->pass) == 0) {
        auth_used = "password";
    } else {
        const char *why = (mode == TARGET_AUTH_KEY)     ? "\r\n[tab5] publickey auth failed (key-only)\r\n"
                        : (mode == TARGET_AUTH_PASSWORD) ? "\r\n[tab5] password auth failed\r\n"
                        : pem ? "\r\n[tab5] auth failed (publickey + password)\r\n"
                              : "\r\n[tab5] auth failed\r\n";
        feed_str(ss->term, why);
        goto out_session;
    }
    snprintf(msg, sizeof(msg), "[tab5] auth: %s\r\n", auth_used);
    feed_str(ss->term, msg);

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(session);
    if (!channel) goto out_session;

    libssh2_channel_request_pty_ex(channel, "xterm-256color", 14,
                                   NULL, 0, term_cols(ss->term), TERM_ROWS, 0, 0);

    if (strlen(t->cmd) > 0) {
        if (libssh2_channel_exec(channel, t->cmd) != 0) goto out_channel;
    } else {
        if (libssh2_channel_shell(channel) != 0) goto out_channel;
    }

    ESP_LOGI(TAG, "shell open: %s@%s", t->user, t->host);
    ss->state = SSH_STATE_CONNECTED;
    ss->connected = true;
    xStreamBufferReset(ss->tx);
    libssh2_session_set_blocking(session, 0);

    uint8_t buf[RX_CHUNK];
    while (!libssh2_channel_eof(channel)) {
        if (ss->closing) break;
        if (ss->pending_resize) {
            ss->pending_resize = false;
            libssh2_channel_request_pty_size(channel, term_cols(ss->term), TERM_ROWS);
        }
        bool idle = true;

        g_dbg_ssh_loops++;
        ssize_t n = libssh2_channel_read(channel, (char *)buf, sizeof(buf));
        if (n > 0) {
            g_dbg_ssh_rx += n;
            term_feed(ss->term, buf, n);
            idle = false;
        } else if (n != LIBSSH2_ERROR_EAGAIN && n < 0) {
            break;
        }

        size_t txn = xStreamBufferReceive(ss->tx, buf, sizeof(buf), 0);
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
    ss->connected = false;
    ss->state = SSH_STATE_CONNECTING;   // task immediately retries (or exits)
    libssh2_session_set_blocking(session, 1);
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
out_session:
    libssh2_session_disconnect(session, "bye");
    libssh2_session_free(session);
out_sock:
    close(sock);
}

// Connect-retry loop, one task per session. PSRAM stack: must not touch
// flash (settings_save runs in the callers' contexts, never here).
static void ssh_session_task(void *arg)
{
    ssh_session_t *ss = arg;
    int id = (int)(ss - s_sessions);

    int backoff_s = 1;
    while (!ss->closing) {
        // Boot with no targets yet: wait for the web config to add one
        // (session 0 only; others can't be opened without a target).
        if (s_cfg->n_targets == 0) {
            feed_str(ss->term, "\r\n[tab5] no SSH targets - add one via the web config\r\n");
            ss->state = SSH_STATE_IDLE;
            while (s_cfg->n_targets == 0 && !ss->closing) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (ss->closing) break;
        }
        if (ss->target >= s_cfg->n_targets) ss->target = 0;
        const ssh_target_t *t = &s_cfg->targets[ss->target];

        char banner[160];
        snprintf(banner, sizeof(banner), "\r\n[tab5] connecting to %s@%s:%d ...\r\n",
                 t->user, t->host, t->port);
        feed_str(ss->term, banner);
        strlcpy(ss->target_name, t->name[0] ? t->name : t->host, sizeof(ss->target_name));
        ss->state = SSH_STATE_CONNECTING;

        session_run(ss, t);

        if (ss->closing) break;
        feed_str(ss->term, "\r\n[tab5] disconnected, retrying...\r\n");
        for (int i = 0; i < backoff_s * 4 && !ss->closing; i++) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (backoff_s < 30) backoff_s *= 2;
    }

    feed_str(ss->term, "\r\n[tab5] session closed\r\n");
    ESP_LOGI(TAG, "session %d closed", id);
    ss->state = SSH_STATE_IDLE;
    ss->connected = false;
    ss->closing = false;
    ss->used = false;          // slot reusable from here on
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------- open/close

int ssh_open(int target_idx)
{
    // target 0 may be opened with no targets configured yet: that's the boot
    // session waiting for the web config (the task idles until one appears).
    if (!s_cfg || target_idx < 0
        || (target_idx >= s_cfg->n_targets && target_idx != 0)) return -1;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Reuse an existing session to the same target: just switch to it.
    for (int i = 0; i < MAX_SSH_SESSIONS; i++) {
        if (s_sessions[i].used && !s_sessions[i].closing
            && s_sessions[i].target == target_idx) {
            xSemaphoreGive(s_lock);
            ssh_set_active(i);
            return i;
        }
    }

    int id = -1;
    for (int i = 0; i < MAX_SSH_SESSIONS; i++) {
        if (!s_sessions[i].used) { id = i; break; }
    }
    if (id < 0) {
        xSemaphoreGive(s_lock);
        feed_active("\r\n[tab5] too many sessions - close one first\r\n");
        return -1;
    }
    if (id > 0 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < MIN_FREE_INTERNAL) {
        xSemaphoreGive(s_lock);
        feed_active("\r\n[tab5] not enough internal RAM for another session\r\n");
        return -1;
    }

    ssh_session_t *ss = &s_sessions[id];
    if (!s_slot_term[id]) {
        s_slot_term[id] = (id == 0) ? s_term0 : term_create();
        if (id != 0) term_set_cols(s_slot_term[id], term_cols(s_term0));
        term_set_response_cb(s_slot_term[id], term_response_cb, (void *)(intptr_t)id);
    } else if (id != 0) {
        // recycled slot: wipe the previous session's leftovers
        feed_str(s_slot_term[id], "\x1b[0m\x1b[2J\x1b[H");
    }
    ss->term = s_slot_term[id];
    if (!ss->tx) ss->tx = xStreamBufferCreate(TX_BUF_SIZE, 1);
    xStreamBufferReset(ss->tx);
    ss->target = target_idx;
    ss->connected = false;
    ss->closing = false;
    ss->pending_resize = false;
    ss->state = SSH_STATE_CONNECTING;
    strlcpy(ss->target_name,
            (s_cfg->n_targets > 0 && s_cfg->targets[target_idx].name[0])
                ? s_cfg->targets[target_idx].name
                : (s_cfg->n_targets > 0 ? s_cfg->targets[target_idx].host : ""),
            sizeof(ss->target_name));
    ss->used = true;

    char name[8];
    snprintf(name, sizeof(name), "ssh%d", id);
    if (xTaskCreatePinnedToCoreWithCaps(ssh_session_task, name, SSH_STACK, ss, 5,
                                        NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ss->used = false;
        xSemaphoreGive(s_lock);
        feed_active("\r\n[tab5] session task create failed\r\n");
        return -1;
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "session %d -> target %d", id, target_idx);
    ssh_set_active(id);
    return id;
}

int ssh_close(int id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!valid_id(id) || !s_sessions[id].used || s_sessions[id].closing) {
        xSemaphoreGive(s_lock);
        return ssh_session_count();
    }
    s_sessions[id].closing = true;   // task notices and frees the slot

    int next = -1;
    if (s_active == id) {
        for (int i = 1; i < MAX_SSH_SESSIONS; i++) {
            int c = (id + i) % MAX_SSH_SESSIONS;
            if (s_sessions[c].used && !s_sessions[c].closing) { next = c; break; }
        }
        s_active = -1;               // set_active(next) below, or boot term
    }
    xSemaphoreGive(s_lock);

    if (next >= 0) {
        ssh_set_active(next);
    } else if (s_active < 0) {
        term_render_set_term(s_term0);   // nothing left: back to the boot term
        local_shell_enter();             // keyboard -> local shell
    }
    return ssh_session_count();
}

void ssh_client_request_switch(int index)
{
    ssh_open(index);
}

void ssh_client_start(term_t *t, settings_t *s, ssh_target_changed_cb_t cb)
{
    s_term0 = t;
    s_cfg = s;
    s_changed_cb = cb;
    s_lock = xSemaphoreCreateMutex();

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init failed: %d", rc);
        return;
    }

    // No auto-connect: sessions are opened from the home panel's SSH app
    // (tap a device tile), Ctrl+Alt+1..9 or the local shell.
    if (s->last_target >= s->n_targets) s->last_target = 0;
}
