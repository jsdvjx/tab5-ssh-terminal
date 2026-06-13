// Device SSH identity (RSA-2048, mbedTLS backend has no Ed25519) and
// per-target host-key pinning, both persisted in NVS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SSH_KEYS_NONE,        // no key yet, no generation running
    SSH_KEYS_GENERATING,  // background task is making one
    SSH_KEYS_READY,
} ssh_keys_status_t;

// Load keypair from NVS; if absent, kick off generation in a low-prio
// background task (returns immediately, doesn't block boot).
void ssh_keys_init(void);

ssh_keys_status_t ssh_keys_status(void);

// OpenSSH "ssh-rsa AAAA... tab5" line. Returns false (and an empty buf)
// while still generating / absent.
bool ssh_keys_public_line(char *buf, size_t cap);

// PEM private key for libssh2_userauth_publickey_frommemory; NULL if not
// ready. The pointer stays valid for the process lifetime.
const char *ssh_keys_private_pem(void);

// Wipe the stored keypair and start generating a fresh one.
void ssh_keys_regen(void);

// Wipe the stored keypair (NVS + RAM) without generating a replacement.
// Status becomes SSH_KEYS_NONE; SSH auth falls back to password. No-op
// while a generation task is running.
void ssh_keys_delete(void);

// Replace the device keypair with a user-supplied private key PEM (must be
// NUL-terminated; len = strlen(pem)). RSA only — the libssh2 mbedTLS backend
// rejects everything else at auth time (_libssh2_mbedtls_pub_priv_key:
// "Key type not supported"), so we reject EC/Ed25519/encrypted keys here
// with a clear message instead of failing later. On success the key is
// persisted to NVS and status becomes READY. On failure the old key is
// untouched and `err` (may be NULL) gets a short reason.
// Call from a task with an internal-RAM stack >= 8 KB (mbedTLS parse + NVS).
bool ssh_keys_import_pem(const char *pem, size_t len, char *err, size_t errcap);

// ---- host key pinning (NVS namespace "hostkeys") --------------------------

typedef enum {
    HOSTKEY_NEW,       // no pin existed: fingerprint stored now
    HOSTKEY_MATCH,     // pin exists and matches
    HOSTKEY_MISMATCH,  // pin exists and DIFFERS (old fp returned)
    HOSTKEY_ERROR,
} hostkey_result_t;

// Check (and on first sight pin) the 32-byte SHA256 host-key fingerprint
// for host:port. On HOSTKEY_MISMATCH, old_fp (32 bytes, may be NULL)
// receives the pinned fingerprint.
hostkey_result_t ssh_hostkey_check(const char *host, int port,
                                   const uint8_t *fp, uint8_t *old_fp);

// Remove the pin for host:port. Returns true if one was removed.
bool ssh_hostkey_clear(const char *host, int port);

// Iterate pinned targets: cb("host:port", fp32) per entry.
void ssh_hostkey_list(void (*cb)(const char *target, const uint8_t *fp));

// Helper: base64 of a 32-byte SHA256 fingerprint, OpenSSH style (no '='
// padding). cap should be >= 48.
void ssh_fp_base64(const uint8_t *fp, char *buf, size_t cap);
