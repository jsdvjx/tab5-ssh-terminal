// Runtime settings, persisted in NVS as a versioned blob.
// Holds Wi-Fi credentials and up to MAX_SSH_TARGETS SSH servers.
#pragma once

#include <stdbool.h>

#define MAX_SSH_TARGETS 8

typedef struct {
    char name[24];      // display name for the panel / picker
    char host[64];
    int  port;
    char user[32];
    char pass[64];
    char cmd[128];      // run after login; empty = interactive shell
} ssh_target_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    ssh_target_t targets[MAX_SSH_TARGETS];
    int  n_targets;
    int  last_target;   // index to preselect on boot
} settings_t;

// Loads from NVS. Returns true if a saved config existed; on first boot the
// struct is seeded from the menuconfig defaults (a target is created from
// CONFIG_TAB5_SSH_* if a host is set).
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);
