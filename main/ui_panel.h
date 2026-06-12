// Right-side status / quick-switch panel (LVGL widgets, TAB5_PANEL_W wide).
// All functions are safe to call from any task (they take the LVGL lock).
#pragma once

#include <stdbool.h>
#include "settings.h"

typedef void (*ui_panel_target_cb_t)(int index);   // tap on a target row

void ui_panel_create(void);

// Show/hide the whole panel (Ctrl+Alt+P). Returns the new visibility.
bool ui_panel_toggle(void);

// Wi-Fi status block. ip/ssid may be NULL ("connecting...").
void ui_panel_set_wifi(const char *ssid, const char *ip, int rssi);

// Web config URL line (NULL hides it).
void ui_panel_set_url(const char *url);

// Rebuild the SSH target list. active = index highlighted, -1 none.
void ui_panel_set_targets(const settings_t *s, int active, ui_panel_target_cb_t cb);

// One-line connection state under the target list ("connected", "auth failed"...).
void ui_panel_set_state(const char *text);

// Modal picker: shows `names[0..n)` under `title`, blocks until the user
// taps one, returns its index. Call from a non-LVGL task only.
int ui_panel_pick(const char *title, const char *names[], int n);
