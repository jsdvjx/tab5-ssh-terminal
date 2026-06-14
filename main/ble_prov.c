// BLE Wi-Fi provisioning GATT server. See ble_prov.h for the protocol.
//
// P4 has no radio: the NimBLE *host* runs here, the controller lives in the
// ESP32-C6 behind esp_hosted (HCI over SDIO, "VHCI"). esp_hosted_init() must
// have run before this (wifi_init() does it — the auto-init constructor is
// patched out, see tools/patch_esp_hosted.py).

#include "ble_prov.h"

#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "ble_prov";

#if !CONFIG_BT_NIMBLE_ENABLED

// BT disabled in sdkconfig: keep the rest of the firmware building/running.
esp_err_t ble_prov_init(settings_t *settings, void (*creds_cb)(void))
{
    (void)settings; (void)creds_cb;
    ESP_LOGW(TAG, "NimBLE disabled in sdkconfig; BLE provisioning off");
    return ESP_FAIL;
}
void ble_prov_start(void) {}
void ble_prov_stop(void) {}
bool ble_prov_is_advertising(void) { return false; }
void ble_prov_set_enabled(bool en) { (void)en; }
bool ble_prov_is_enabled(void) { return false; }
void ble_prov_wifi_state(ble_prov_wifi_state_t st, const char *ip)
{
    (void)st; (void)ip;
}

#else // CONFIG_BT_NIMBLE_ENABLED

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ssh_keys.h"
#include "wifi.h"
#include "security.h"

#define DEVICE_NAME        "Tab5-SSH"
#define PREFERRED_MTU      256
// Advertising stays ON permanently (the 5-min window made the device
// impossible to find when reprovisioning; adv power cost is negligible).
// `ble off` in the local shell still stops it manually.
#define ADV_GRACE_US       0   // 0 = never auto-stop after got-IP
#define SCAN_MAX_APS       24
#define SCAN_JSON_CAP      2048

// 128-bit UUIDs, base 5a7eXXXX-91f4-4b9a-aa44-57c5b5e4d50a (little-endian).
#define PROV_UUID(short16) BLE_UUID128_INIT( \
    0x0a, 0xd5, 0xe4, 0xb5, 0xc5, 0x57, 0x44, 0xaa, \
    0x9a, 0x4b, 0xf4, 0x91, (uint8_t)(short16), (uint8_t)((short16) >> 8), \
    0x7e, 0x5a)

static const ble_uuid128_t UUID_SVC    = PROV_UUID(0x0001);
static const ble_uuid128_t UUID_SCAN   = PROV_UUID(0x0002);
static const ble_uuid128_t UUID_CREDS  = PROV_UUID(0x0003);
static const ble_uuid128_t UUID_STATUS = PROV_UUID(0x0004);
static const ble_uuid128_t UUID_KEY    = PROV_UUID(0x0005);
static const ble_uuid128_t UUID_AUTH   = PROV_UUID(0x0006);

static settings_t *s_settings;
static void (*s_creds_cb)(void);

static bool     s_ready;                      // stack came up
static bool     s_enabled = false;            // master on/off (settings.ble_enabled); default OFF
static bool     s_should_advertise;           // desired advertising state
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
// Per-connection PIN auth: the conn_handle that wrote the correct PIN to
// UUID_AUTH. Sensitive writes (creds, key) require this == the writer's
// conn_handle. Cleared on disconnect. SCAN/STATUS stay open.
static uint16_t s_authed_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_scan_val_handle;
static uint16_t s_status_val_handle;
static uint8_t  s_own_addr_type;

static esp_timer_handle_t s_grace_timer;

static ble_prov_wifi_state_t s_wifi_st = BLE_PROV_WIFI_IDLE;
static char s_ip[20];

static char s_scan_json[SCAN_JSON_CAP] = "[]";
static volatile bool s_scan_running;

static void advertise(void);

// ---------------------------------------------------------------- status

static int status_json(char *buf, size_t cap)
{
    static const char *names[] = { "idle", "connecting", "connected", "fail" };
    if (s_wifi_st == BLE_PROV_WIFI_CONNECTED && s_ip[0]) {
        return snprintf(buf, cap, "{\"st\":\"%s\",\"ip\":\"%s\"}",
                        names[s_wifi_st], s_ip);
    }
    return snprintf(buf, cap, "{\"st\":\"%s\"}", names[s_wifi_st]);
}

static void notify_status(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    char buf[64];
    int n = status_json(buf, sizeof(buf));
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
    if (om) ble_gatts_notify_custom(s_conn, s_status_val_handle, om);
}

// ----------------------------------------------------------------- scan

// Background scan; result JSON is chunked over notifications on the SCAN
// characteristic, terminated with a zero-length notification.
static void scan_task(void *arg)
{
    static wifi_ap_record_t recs[SCAN_MAX_APS];
    int n = wifi_scan(recs, SCAN_MAX_APS);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < (n > 0 ? n : 0); i++) {
        if (!recs[i].ssid[0]) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "s", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "r", recs[i].rssi);
        cJSON_AddNumberToObject(o, "a", recs[i].authmode == WIFI_AUTH_OPEN ? 0 : 1);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (json) {
        strlcpy(s_scan_json, json, sizeof(s_scan_json));
        cJSON_free(json);
    }

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        // MTU-aware chunking: payload per notification is ATT_MTU - 3.
        int chunk = ble_att_mtu(s_conn) - 3;
        if (chunk < 20) chunk = 20;
        size_t len = strlen(s_scan_json);
        for (size_t off = 0; off < len; off += chunk) {
            size_t take = len - off > (size_t)chunk ? (size_t)chunk : len - off;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(s_scan_json + off, take);
            if (!om || ble_gatts_notify_custom(s_conn, s_scan_val_handle, om) != 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(15));  // don't flood hosted SDIO queue
        }
        struct os_mbuf *end = ble_hs_mbuf_from_flat("", 0);   // end marker
        if (end) ble_gatts_notify_custom(s_conn, s_scan_val_handle, end);
    }

    s_scan_running = false;
    vTaskDelete(NULL);
}

static void kick_scan(void)
{
    if (s_scan_running) return;
    s_scan_running = true;
    if (xTaskCreate(scan_task, "ble_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scan_running = false;
    }
}

// ----------------------------------------------------------------- creds

static int handle_creds_write(struct os_mbuf *om)
{
    char buf[200];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf) - 1, &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = 0;

    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "s");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
    if (!cJSON_IsString(s) || !s->valuestring[0]) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }

    strlcpy(s_settings->wifi_ssid, s->valuestring, sizeof(s_settings->wifi_ssid));
    strlcpy(s_settings->wifi_pass, cJSON_IsString(p) ? p->valuestring : "",
            sizeof(s_settings->wifi_pass));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "creds via BLE: ssid '%s'", s_settings->wifi_ssid);
    settings_save(s_settings);          // same NVS blob settings.c always uses

    s_wifi_st = BLE_PROV_WIFI_CONNECTING;
    notify_status();
    if (s_creds_cb) s_creds_cb();       // app decides who runs the connect
    return 0;
}

// ------------------------------------------------------------------- key
// SSH private-key upload. A PEM (~1.7KB for RSA-2048) exceeds one ATT MTU
// and Web Bluetooth sends each writeValue() as its OWN Write Request — there
// is no cross-write reassembly to rely on. So the protocol is accumulate-
// until-END: a chunk starting with "-----BEGIN" resets the buffer, chunks
// append, and once "-----END ...PRIVATE KEY-----" is seen the whole buffer
// goes to a worker task. (NimBLE's prepare/execute long-writes also land
// here as one big flat write — handled by the same path.)
//
// The import itself (mbedTLS parse + NVS write) does NOT run on the NimBLE
// host task: that stack is only CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096.
// It is internal RAM (plain xTaskCreatePinnedToCore), so NVS would be safe,
// but the stack is too tight for pk_parse — a one-shot 8KB worker does it.
// Result is surfaced as {"key":"imported"|"key_error: ..."} on STATUS.

#define KEY_PEM_CAP 2300

static char   s_key_buf[KEY_PEM_CAP];
static size_t s_key_len;
static volatile bool s_key_importing;

static void notify_key_result(const char *msg)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "{\"key\":\"%s\"}", msg);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
    if (om) ble_gatts_notify_custom(s_conn, s_status_val_handle, om);
}

static void key_import_task(void *arg)
{
    char err[96];
    bool ok = ssh_keys_import_pem(s_key_buf, s_key_len, err, sizeof(err));
    if (ok) {
        notify_key_result("imported");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "key_error: %s", err);
        ESP_LOGW(TAG, "%s", msg);
        notify_key_result(msg);
    }
    memset(s_key_buf, 0, sizeof(s_key_buf));
    s_key_len = 0;
    s_key_importing = false;
    vTaskDelete(NULL);
}

static int handle_key_write(struct os_mbuf *om)
{
    if (s_key_importing) return BLE_ATT_ERR_INSUFFICIENT_RES;

    char chunk[512];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, chunk, sizeof(chunk), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (len >= 10 && memcmp(chunk, "-----BEGIN", 10) == 0) s_key_len = 0;
    if (s_key_len + len + 1 > sizeof(s_key_buf)) {
        s_key_len = 0;
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    memcpy(s_key_buf + s_key_len, chunk, len);
    s_key_len += len;
    s_key_buf[s_key_len] = 0;

    if (strstr(s_key_buf, "-----END ") &&
        strstr(s_key_buf, "PRIVATE KEY-----")) {
        s_key_importing = true;
        if (xTaskCreate(key_import_task, "keyimport", 8192, NULL, 4,
                        NULL) != pdPASS) {
            s_key_importing = false;
            s_key_len = 0;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ auth
// The client writes the 8-char device PIN to UUID_AUTH. On a match THIS
// connection (conn_handle) is marked authenticated; creds + key writes are
// rejected until then and the authentication is dropped on disconnect.

static int handle_auth_write(uint16_t conn_handle, struct os_mbuf *om)
{
    char buf[32];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf) - 1, &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = 0;
    if (!security_check(buf)) {
        s_authed_conn = BLE_HS_CONN_HANDLE_NONE;
        if (s_conn == conn_handle) {
            struct os_mbuf *m = ble_hs_mbuf_from_flat("{\"err\":\"auth\"}", 14);
            if (m) ble_gatts_notify_custom(s_conn, s_status_val_handle, m);
        }
        ESP_LOGW(TAG, "BLE auth failed");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    s_authed_conn = conn_handle;
    ESP_LOGI(TAG, "BLE connection authenticated");
    if (s_conn == conn_handle) {
        struct os_mbuf *m = ble_hs_mbuf_from_flat("{\"auth\":\"ok\"}", 13);
        if (m) ble_gatts_notify_custom(s_conn, s_status_val_handle, m);
    }
    return 0;
}

// ------------------------------------------------------------------ gatt

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ble_uuid_cmp(uuid, &UUID_SCAN.u) == 0) {
            // A read kicks a fresh scan and returns the cached result; the
            // fresh list arrives via notifications when the scan finishes.
            kick_scan();
            return os_mbuf_append(ctxt->om, s_scan_json, strlen(s_scan_json))
                       ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        if (ble_uuid_cmp(uuid, &UUID_STATUS.u) == 0) {
            char buf[64];
            int n = status_json(buf, sizeof(buf));
            return os_mbuf_append(ctxt->om, buf, n)
                       ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        if (ble_uuid_cmp(uuid, &UUID_KEY.u) == 0) {
            // Current device PUBLIC line (paste into authorized_keys), or a
            // bare status word while no key exists yet.
            char line[640];
            if (!ssh_keys_public_line(line, sizeof(line))) {
                strlcpy(line, ssh_keys_status() == SSH_KEYS_GENERATING
                              ? "generating" : "none", sizeof(line));
            }
            return os_mbuf_append(ctxt->om, line, strlen(line))
                       ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (ble_uuid_cmp(uuid, &UUID_AUTH.u) == 0) {
            return handle_auth_write(conn_handle, ctxt->om);
        }
        // Sensitive writes require this connection to be PIN-authenticated.
        bool authed = (s_authed_conn != BLE_HS_CONN_HANDLE_NONE
                       && s_authed_conn == conn_handle);
        if (ble_uuid_cmp(uuid, &UUID_CREDS.u) == 0) {
            if (!authed) {
                struct os_mbuf *m = ble_hs_mbuf_from_flat("{\"err\":\"auth\"}", 14);
                if (m && s_conn != BLE_HS_CONN_HANDLE_NONE)
                    ble_gatts_notify_custom(s_conn, s_status_val_handle, m);
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            return handle_creds_write(ctxt->om);
        }
        if (ble_uuid_cmp(uuid, &UUID_KEY.u) == 0) {
            if (!authed) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            return handle_key_write(ctxt->om);
        }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_SCAN.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_scan_val_handle,
            }, {
                .uuid = &UUID_CREDS.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            }, {
                .uuid = &UUID_STATUS.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
            }, {
                .uuid = &UUID_KEY.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            }, {
                .uuid = &UUID_AUTH.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 }
        },
    },
    { 0 }
};

// ------------------------------------------------------------------- gap

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected");
        } else if (s_should_advertise) {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason %d)",
                 event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_authed_conn = BLE_HS_CONN_HANDLE_NONE;   // drop PIN auth on disconnect
        if (s_should_advertise) advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_should_advertise) advertise();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (ble_gap_adv_active()) return;

    // ADV packet: flags + the 128-bit service UUID (needed so Web Bluetooth
    // can filter on it). Name goes in the scan response (31-byte limit).
    struct ble_hs_adv_fields adv = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = (ble_uuid128_t *)&UUID_SVC,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .name = (const uint8_t *)DEVICE_NAME,
        .name_len = strlen(DEVICE_NAME),
        .name_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc == 0) {
        struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
        };
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &params, gap_event, NULL);
    }
    if (rc != 0) ESP_LOGW(TAG, "adv start failed; rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as '%s'", DEVICE_NAME);
}

void ble_prov_start(void)
{
    if (!s_ready || !s_enabled) return;
    s_should_advertise = true;
    advertise();
}

void ble_prov_set_enabled(bool en)
{
    s_enabled = en;
    if (!en) {
        ble_prov_stop();            // stop adv + clear s_should_advertise
    } else if (s_wifi_st != BLE_PROV_WIFI_CONNECTED) {
        ble_prov_start();           // only advertise when not already on Wi-Fi
    }
}

bool ble_prov_is_enabled(void)
{
    return s_enabled;
}

void ble_prov_stop(void)
{
    if (!s_ready) return;
    s_should_advertise = false;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    ESP_LOGI(TAG, "advertising stopped");
}

bool ble_prov_is_advertising(void)
{
    return s_ready && ble_gap_adv_active();
}

static void grace_timer_cb(void *arg)
{
    if (s_wifi_st == BLE_PROV_WIFI_CONNECTED) {
        ESP_LOGI(TAG, "re-provisioning grace period over");
        ble_prov_stop();
    }
}

// --------------------------------------------------------- wifi.c hook

void ble_prov_wifi_state(ble_prov_wifi_state_t st, const char *ip)
{
    if (!s_ready) return;
    if (st == s_wifi_st && st != BLE_PROV_WIFI_CONNECTED) return;

    s_wifi_st = st;
    if (st == BLE_PROV_WIFI_CONNECTED && ip) {
        strlcpy(s_ip, ip, sizeof(s_ip));
    } else if (st != BLE_PROV_WIFI_CONNECTED) {
        s_ip[0] = 0;
    }
    notify_status();

    if (st == BLE_PROV_WIFI_CONNECTED) {
        // keep advertising 5 more minutes, then free the airtime
        esp_timer_stop(s_grace_timer);
        if (ADV_GRACE_US > 0) esp_timer_start_once(s_grace_timer, ADV_GRACE_US);
    } else if (st == BLE_PROV_WIFI_FAIL || st == BLE_PROV_WIFI_IDLE) {
        esp_timer_stop(s_grace_timer);
        ble_prov_start();
    }
}

// ------------------------------------------------------------------ init

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGW(TAG, "no usable BT address");
        return;
    }
    s_ready = true;
    if (s_should_advertise) advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset; reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();                 // returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

esp_err_t ble_prov_init(settings_t *settings, void (*creds_cb)(void))
{
    s_settings = settings;
    s_creds_cb = creds_cb;
    s_enabled = settings ? settings->ble_enabled : false;

    // HCI rides the esp_hosted SDIO transport to the C6; if its firmware
    // lacks BT (capabilities say "hci_stub"), bring-up fails -> warn + skip.
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble_port_init failed (%s) — BLE provisioning off "
                      "(co-processor firmware without BT?)",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }

    const esp_timer_create_args_t targs = {
        .callback = grace_timer_cb,
        .name = "ble_grace",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_grace_timer));

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;   // Just Works
    ble_hs_cfg.sm_bonding = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "GATT registration failed; rc=%d", rc);
        return ESP_FAIL;
    }
    ble_att_set_preferred_mtu(PREFERRED_MTU);
    ble_store_config_init();

    // Wi-Fi is not connected at this point — advertise unless disabled. The
    // on_sync callback starts advertising once the host is ready.
    s_should_advertise = s_enabled;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE provisioning up");
    return ESP_OK;
}

#endif // CONFIG_BT_NIMBLE_ENABLED
