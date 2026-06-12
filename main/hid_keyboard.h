#pragma once
#include "term.h"

// USB HID host: external keyboard on the Tab5 USB-A port.
// Translates keycodes to bytes/escape sequences and pushes them to the
// current sink. Needs `t` for DECCKM-aware arrow-key encoding.
void hid_keyboard_start(term_t *t);

// Where key bytes go: ssh_client_send() normally, setup_key_input() during
// on-device setup. Default sink is ssh_client_send.
typedef void (*hid_key_sink_t)(const uint8_t *data, size_t len);
void hid_keyboard_set_sink(hid_key_sink_t sink);

// Translate one HID (usage, modifiers) press to terminal bytes and push to
// the current sink. Shared by the USB HID driver and the I2C Tab5 Keyboard
// (which reports the same HID codes in HID mode).
void hid_send_key(uint8_t usage, uint8_t modifiers);

// Local hotkey: Ctrl+Alt+P is swallowed (never sent to SSH) and triggers
// this callback instead — used to toggle the side panel.
typedef void (*hid_hotkey_cb_t)(void);
void hid_keyboard_set_hotkey_cb(hid_hotkey_cb_t cb);
