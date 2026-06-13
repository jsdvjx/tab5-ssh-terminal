// Device SSH identity + host-key pinning.
//
// Key type: RSA-2048. The libssh2 mbedTLS crypto backend has no Ed25519
// (LIBSSH2_ED25519 == 0 in mbedtls.h), and RSA is the type its
// userauth_publickey_frommemory path is solid on. Generation takes tens of
// seconds on the P4, so it runs ONCE in a low-prio background task at first
// boot and the result is persisted in NVS ("sshkeys" namespace).
//
// Host-key pins live in their own NVS namespace ("hostkeys"). NVS keys are
// max 15 chars, so the key is 'h' + 14 hex chars of SHA256("host:port");
// the value blob stores the 32-byte fingerprint followed by the readable
// "host:port" string so `hostkey` can list pins.

#include "ssh_keys.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ssh_keys";

#define KEY_BITS      2048
#define PEM_CAP       2048           // RSA-2048 PKCS#1 PEM is ~1700 bytes
#define PUB_CAP       640            // "ssh-rsa " + b64(~280B blob) + " tab5"
#define NS_KEYS       "sshkeys"
#define NS_HOSTKEYS   "hostkeys"
#define KEY_PRIV      "priv"
#define KEY_PUB       "pub"

static char s_pem[PEM_CAP];          // private key PEM (NUL-terminated)
static char s_pub[PUB_CAP];          // OpenSSH public line
static volatile ssh_keys_status_t s_status = SSH_KEYS_NONE;

// ---------------------------------------------------------------- helpers

static bool keys_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_KEYS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, KEY_PRIV, s_pem, strlen(s_pem) + 1) == ESP_OK
           && nvs_set_str(h, KEY_PUB, s_pub) == ESP_OK
           && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool keys_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS_KEYS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t plen = sizeof(s_pem), blen = sizeof(s_pub);
    bool ok = nvs_get_blob(h, KEY_PRIV, s_pem, &plen) == ESP_OK
           && nvs_get_str(h, KEY_PUB, s_pub, &blen) == ESP_OK;
    nvs_close(h);
    if (!ok) { s_pem[0] = s_pub[0] = 0; }
    return ok;
}

// Append an SSH-wire mpint (length-prefixed big-endian, extra zero byte if
// the high bit is set) of mbedtls MPI X into buf at *off.
static int put_mpint(uint8_t *buf, size_t cap, size_t *off, const mbedtls_mpi *X)
{
    size_t n = mbedtls_mpi_size(X);
    uint8_t tmp[260];
    if (n > sizeof(tmp)) return -1;
    if (mbedtls_mpi_write_binary(X, tmp, n) != 0) return -1;
    int pad = (n > 0 && (tmp[0] & 0x80)) ? 1 : 0;
    uint32_t len = n + pad;
    if (*off + 4 + len > cap) return -1;
    buf[(*off)++] = len >> 24; buf[(*off)++] = len >> 16;
    buf[(*off)++] = len >> 8;  buf[(*off)++] = len;
    if (pad) buf[(*off)++] = 0;
    memcpy(buf + *off, tmp, n);
    *off += n;
    return 0;
}

// Build "ssh-rsa AAAA... tab5" from the pk context into s_pub.
static bool make_public_line(mbedtls_pk_context *pk)
{
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
    mbedtls_mpi N, E;
    mbedtls_mpi_init(&N);
    mbedtls_mpi_init(&E);
    bool ok = false;
    uint8_t blob[300];
    size_t off = 0;

    if (mbedtls_rsa_export(rsa, &N, NULL, NULL, NULL, &E) != 0) goto out;
    static const char type[] = "ssh-rsa";
    uint32_t tl = sizeof(type) - 1;
    blob[off++] = tl >> 24; blob[off++] = tl >> 16;
    blob[off++] = tl >> 8;  blob[off++] = tl;
    memcpy(blob + off, type, tl); off += tl;
    if (put_mpint(blob, sizeof(blob), &off, &E) != 0) goto out;
    if (put_mpint(blob, sizeof(blob), &off, &N) != 0) goto out;

    size_t b64len = 0;
    char b64[440];
    if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &b64len,
                              blob, off) != 0) goto out;
    b64[b64len] = 0;
    snprintf(s_pub, sizeof(s_pub), "ssh-rsa %s tab5", b64);
    ok = true;
out:
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&E);
    return ok;
}

// ---------------------------------------------------------------- generation

static void keygen_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "generating RSA-%d key (background, ~10-60s)...", KEY_BITS);

    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)"tab5-sshkey", 11);
    if (rc == 0) rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc == 0) rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk),
                                          mbedtls_ctr_drbg_random, &drbg,
                                          KEY_BITS, 65537);
    if (rc == 0) rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)s_pem,
                                               sizeof(s_pem));
    if (rc == 0 && !make_public_line(&pk)) rc = -1;

    if (rc == 0) {
        if (!keys_save()) ESP_LOGE(TAG, "NVS save failed (key kept in RAM)");
        s_status = SSH_KEYS_READY;
        ESP_LOGI(TAG, "RSA-%d key ready in %llds (run `sshkey` for the public line)",
                 KEY_BITS, (long long)((esp_timer_get_time() - t0) / 1000000));
    } else {
        s_pem[0] = s_pub[0] = 0;
        s_status = SSH_KEYS_NONE;
        ESP_LOGE(TAG, "key generation failed: -0x%04x", -rc);
    }

    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    vTaskDelete(NULL);
}

static void start_keygen(void)
{
    s_status = SSH_KEYS_GENERATING;
    // Prio 1 (just above idle): RSA keygen is pure CPU, let everything else win.
    if (xTaskCreate(keygen_task, "sshkeygen", 8192, NULL, 1, NULL) != pdPASS) {
        s_status = SSH_KEYS_NONE;
        ESP_LOGE(TAG, "keygen task create failed");
    }
}

// ---------------------------------------------------------------- public API

static void pins_load(void);

void ssh_keys_init(void)
{
    pins_load();   // host-key pin RAM cache + persist worker
    if (keys_load()) {
        s_status = SSH_KEYS_READY;
        ESP_LOGI(TAG, "loaded RSA key from NVS");
    } else {
        start_keygen();
    }
}

ssh_keys_status_t ssh_keys_status(void)
{
    return s_status;
}

bool ssh_keys_public_line(char *buf, size_t cap)
{
    if (s_status != SSH_KEYS_READY) {
        if (cap) buf[0] = 0;
        return false;
    }
    strlcpy(buf, s_pub, cap);
    return true;
}

const char *ssh_keys_private_pem(void)
{
    return (s_status == SSH_KEYS_READY) ? s_pem : NULL;
}

void ssh_keys_regen(void)
{
    if (s_status == SSH_KEYS_GENERATING) return;
    nvs_handle_t h;
    if (nvs_open(NS_KEYS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_pem[0] = s_pub[0] = 0;
    start_keygen();
}

void ssh_keys_delete(void)
{
    if (s_status == SSH_KEYS_GENERATING) return;
    nvs_handle_t h;
    if (nvs_open(NS_KEYS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_pem[0] = s_pub[0] = 0;
    s_status = SSH_KEYS_NONE;
}

bool ssh_keys_import_pem(const char *pem, size_t len, char *err, size_t errcap)
{
    if (err && errcap) err[0] = 0;
#define IMPORT_FAIL(msg) do { \
        if (err && errcap) strlcpy(err, (msg), errcap); \
        goto out; } while (0)

    bool ok = false;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    if (s_status == SSH_KEYS_GENERATING)
        IMPORT_FAIL("busy: key generation in progress");
    if (len + 1 > PEM_CAP)
        IMPORT_FAIL("key too large (RSA-2048 PEM max ~1.8KB)");
    if (strstr(pem, "BEGIN OPENSSH PRIVATE KEY"))
        IMPORT_FAIL("OpenSSH format: convert with `ssh-keygen -p -m PEM` first");
    if (strstr(pem, "ENCRYPTED"))
        IMPORT_FAIL("encrypted key not supported, remove the passphrase");

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)"tab5-import", 11) != 0)
        IMPORT_FAIL("rng init failed");

    int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)pem, len + 1,
                                  NULL, 0, mbedtls_ctr_drbg_random, &drbg);
    if (rc == MBEDTLS_ERR_PK_PASSWORD_REQUIRED)
        IMPORT_FAIL("encrypted key not supported, remove the passphrase");
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "PEM parse failed (-0x%04x)", -rc);
        IMPORT_FAIL(msg);
    }
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA)
        IMPORT_FAIL("only RSA keys work (libssh2 mbedTLS backend "
                    "rejects EC/Ed25519 at auth)");

    // Re-emit a normalized PKCS#1 PEM so libssh2 always sees the same shape.
    char pem_tmp[PEM_CAP];
    if (mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem_tmp,
                                 sizeof(pem_tmp)) != 0)
        IMPORT_FAIL("key too large for storage (use RSA-2048)");

    char pub_old[PUB_CAP];
    memcpy(pub_old, s_pub, sizeof(pub_old));
    char pem_old[PEM_CAP];
    memcpy(pem_old, s_pem, sizeof(pem_old));

    memcpy(s_pem, pem_tmp, sizeof(s_pem));
    if (!make_public_line(&pk)) {          // writes s_pub
        memcpy(s_pem, pem_old, sizeof(s_pem));
        memcpy(s_pub, pub_old, sizeof(s_pub));
        IMPORT_FAIL("public line build failed");
    }
    if (!keys_save()) {
        ESP_LOGE(TAG, "NVS save failed (imported key kept in RAM)");
    }
    s_status = SSH_KEYS_READY;
    ESP_LOGI(TAG, "imported RSA-%u key", (unsigned)mbedtls_pk_get_bitlen(&pk));
    ok = true;
out:
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&pk);
    return ok;
#undef IMPORT_FAIL
}

// ---------------------------------------------------------------- pinning

static void target_str(const char *host, int port, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s:%d", host, port);
}

// NVS key: 'h' + first 7 bytes of SHA256("host:port") as hex (15 chars).
static void nvs_key_for(const char *target, char *key16)
{
    uint8_t md[32];
    mbedtls_sha256((const unsigned char *)target, strlen(target), md, 0);
    key16[0] = 'h';
    for (int i = 0; i < 7; i++) sprintf(key16 + 1 + 2 * i, "%02x", md[i]);
    key16[15] = 0;
}

// RAM pin cache + persist worker. ssh_hostkey_check() runs on SSH session
// tasks whose stacks live in PSRAM — ANY spi_flash access (even NVS reads)
// disables cache and asserts on a PSRAM-stack task (cache_utils.c:127, the
// 2026-06-13 boot loop). So: all pins are loaded into RAM at init, checks
// touch RAM only, and first-sight pins are persisted by a small worker task
// with an internal-RAM stack.
#define MAX_PINS 16

typedef struct {
    char target[80];
    uint8_t fp[32];
} pin_t;

static pin_t s_pin_ram[MAX_PINS];
static int s_pin_count;
static SemaphoreHandle_t s_pin_mu;
static QueueHandle_t s_pin_q;          // pin_t copies to persist

static bool pin_persist_nvs(const pin_t *p)
{
    char key[16];
    nvs_key_for(p->target, key);
    nvs_handle_t h;
    if (nvs_open(NS_HOSTKEYS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t blob[32 + 80];
    memcpy(blob, p->fp, 32);
    size_t tlen = strlen(p->target) + 1;
    memcpy(blob + 32, p->target, tlen);
    bool ok = nvs_set_blob(h, key, blob, 32 + tlen) == ESP_OK
           && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void pin_worker(void *arg)
{
    pin_t p;
    while (xQueueReceive(s_pin_q, &p, portMAX_DELAY) == pdTRUE) {
        if (!pin_persist_nvs(&p)) {
            ESP_LOGE(TAG, "pin persist failed for %s", p.target);
        } else {
            ESP_LOGI(TAG, "pinned %s", p.target);
        }
    }
}

// Load every stored pin into the RAM cache. Called once from ssh_keys_init
// (main task, internal stack — flash access is fine there).
static void pins_load(void)
{
    s_pin_mu = xSemaphoreCreateMutex();
    s_pin_q = xQueueCreate(4, sizeof(pin_t));
    xTaskCreate(pin_worker, "pinworker", 4096, NULL, 3, NULL);

    nvs_handle_t h;
    if (nvs_open(NS_HOSTKEYS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NS_HOSTKEYS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && s_pin_count < MAX_PINS) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint8_t blob[32 + 80];
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, info.key, blob, &len) == ESP_OK && len > 33) {
            pin_t *p = &s_pin_ram[s_pin_count++];
            memcpy(p->fp, blob, 32);
            strlcpy(p->target, (const char *)blob + 32, sizeof(p->target));
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    ESP_LOGI(TAG, "%d host-key pin(s) loaded", s_pin_count);
}

hostkey_result_t ssh_hostkey_check(const char *host, int port,
                                   const uint8_t *fp, uint8_t *old_fp)
{
    char target[80];
    target_str(host, port, target, sizeof(target));
    if (!s_pin_mu) return HOSTKEY_ERROR;

    xSemaphoreTake(s_pin_mu, portMAX_DELAY);
    for (int i = 0; i < s_pin_count; i++) {
        if (strcmp(s_pin_ram[i].target, target) == 0) {
            bool match = memcmp(s_pin_ram[i].fp, fp, 32) == 0;
            if (!match && old_fp) memcpy(old_fp, s_pin_ram[i].fp, 32);
            xSemaphoreGive(s_pin_mu);
            return match ? HOSTKEY_MATCH : HOSTKEY_MISMATCH;
        }
    }
    if (s_pin_count >= MAX_PINS) {
        xSemaphoreGive(s_pin_mu);
        return HOSTKEY_ERROR;
    }
    // First sight: pin in RAM immediately, persist via the worker.
    pin_t *p = &s_pin_ram[s_pin_count++];
    strlcpy(p->target, target, sizeof(p->target));
    memcpy(p->fp, fp, 32);
    pin_t copy = *p;
    xSemaphoreGive(s_pin_mu);
    xQueueSend(s_pin_q, &copy, 0);
    return HOSTKEY_NEW;
}

bool ssh_hostkey_clear(const char *host, int port)
{
    char target[80], key[16];
    target_str(host, port, target, sizeof(target));
    nvs_key_for(target, key);

    xSemaphoreTake(s_pin_mu, portMAX_DELAY);
    for (int i = 0; i < s_pin_count; i++) {
        if (strcmp(s_pin_ram[i].target, target) == 0) {
            s_pin_ram[i] = s_pin_ram[--s_pin_count];
            break;
        }
    }
    xSemaphoreGive(s_pin_mu);

    nvs_handle_t h;
    if (nvs_open(NS_HOSTKEYS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_erase_key(h, key) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void ssh_hostkey_list(void (*cb)(const char *target, const uint8_t *fp))
{
    xSemaphoreTake(s_pin_mu, portMAX_DELAY);
    for (int i = 0; i < s_pin_count; i++) {
        cb(s_pin_ram[i].target, s_pin_ram[i].fp);
    }
    xSemaphoreGive(s_pin_mu);
}

void ssh_fp_base64(const uint8_t *fp, char *buf, size_t cap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)buf, cap, &olen, fp, 32) != 0) {
        if (cap) buf[0] = 0;
        return;
    }
    while (olen > 0 && buf[olen - 1] == '=') olen--;   // OpenSSH drops padding
    buf[olen] = 0;
}
