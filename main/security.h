// Device PIN: gates the web config server (HTTP Basic Auth) and BLE
// provisioning (AUTH characteristic). Generated once at first boot from an
// unambiguous alphabet (no 0/O/1/I/L), persisted in its own NVS key, logged
// to serial and shown on screen (System card).
#pragma once

#include <stdbool.h>

#define SECURITY_PIN_LEN 8

// Loads the PIN from NVS, generating + persisting a fresh random one if none
// exists. Call once at boot before web_config_start()/ble_prov_init().
void security_init(void);

// 8-char NUL-terminated PIN (valid after security_init()).
const char *security_pin(void);

// Generate a new random PIN and persist it. Safe to call at runtime.
void security_regen_pin(void);

// Constant-time-ish compare of `candidate` against the PIN. Returns true on
// match. NULL or wrong-length candidates return false.
bool security_check(const char *candidate);
