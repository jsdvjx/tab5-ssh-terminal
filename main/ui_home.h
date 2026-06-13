// Full-screen home panel (Ctrl+Alt+P): card grid with SSH targets, Wi-Fi,
// BLE provisioning, display, system and IME status. Replaces the old
// right-side ui_panel for the SSH phase (ui_panel stays for boot/setup).
// All functions are safe to call from any task (they take the LVGL lock).
#pragma once

#include <stdbool.h>
#include "settings.h"

// Build the (hidden) panel. Call after assets_load() (CJK font) and
// status_bar_init(). `s` must outlive the panel (app_main's static).
void ui_home_create(settings_t *s);

// Ctrl+Alt+P / Esc. Returns the new visibility.
bool ui_home_toggle(void);

// True while the panel covers the screen (hid_keyboard swallows keys then).
bool ui_home_is_open(void);

// Force a state (UI-dev helper for GET /shot?full=1&panel=1).
void ui_home_set_open(bool open);

// SSH app view (UI-dev helper for GET /shot?...&edit=N):
// 0 = card grid, 1 = expanded SSH app (device tiles + dock),
// 2 = blank add-target form. Opens the panel if needed. Takes the LVGL lock.
void ui_home_show_sheet(int target_idx);
void ui_home_set_edit_view(int mode);

// SSH key panel (UI-dev helper for GET /shot?...&keys=1): 1 = open the key
// panel over the expanded SSH app, 0 = back to the SSH app. Opens the panel
// if needed. Takes the LVGL lock.
void ui_home_set_keys_view(int on);

// 连接 app view (UI-dev helper for GET /shot?...&conn=N):
// 0 = card grid, 1 = expanded 连接 app (wifi detail + saved list + BT),
// 2 = 添加网络 mini-form. Opens the panel if needed. Takes the LVGL lock.
void ui_home_set_conn_view(int mode);

// UI-DEV HELPER for GET /shot?...&lang=0|1: force the UI language (0 = 中文,
// 1 = English) and live-rebuild the panel before the snapshot. Does NOT persist
// to settings — purely a snapshot helper. Takes the LVGL lock.
void ui_home_set_lang_dev(int lang);

// Hardware-keyboard routing while the panel is open. Called by
// hid_send_key() BEFORE its swallow: returns true if the key was consumed
// (editor open: printables type into the focused textarea, Backspace
// deletes, Tab/Enter advance fields, Esc backs out one level).
bool ui_home_handle_key(uint8_t usage, uint8_t modifiers);

// Targets changed elsewhere (web config POST, ssh open/switch): rebuild the
// SSH rows/tiles. Safe from any task; no-op while the panel is closed.
void ui_home_refresh(void);
