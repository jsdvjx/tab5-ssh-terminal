// On-device interactive setup flows. Keyboard input arrives via
// setup_key_input (wired through hid_keyboard_set_sink).
#pragma once

#include <stdbool.h>
#include "term.h"
#include "settings.h"

// Keyboard sink while in setup mode.
void setup_key_input(const uint8_t *data, size_t len);

// Shows "press 's' for setup" for `timeout_ms`; returns true if user wants in.
bool setup_offered(term_t *t, int timeout_ms);

// Wi-Fi onboarding: scan -> touch-pick SSID -> type password -> save to NVS.
// Returns false if the scan found nothing (caller may retry).
bool setup_wifi(term_t *t, settings_t *s);
