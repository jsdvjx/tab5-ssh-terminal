// Screen-sleep power management: blanks the backlight after an idle timeout
// (no keyboard or touch input) and covers the UI with a black overlay that
// swallows the wake tap. SSH/Wi-Fi keep running while asleep. Also hosts the
// firmware power-off pulse (IO expander #2 pin 4 -> PMIC latch).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PM_IDLE_TIMEOUT_S 180

// Create the (hidden) wake overlay and start the 1s idle-check timer.
// Call after the display + status bar are up (needs the LVGL top layer).
void power_mgmt_init(void);

// Enable the IP2326 battery charger: CHG_EN (IO expander 0x44 bit 7) powers
// up LOW, so the battery never charges until firmware sets it.
void power_mgmt_charge_enable(void);

// Called from hid_send_key() after the panel hotkey, before the IME filter.
// Returns true if the key was consumed: any key while asleep (wakes the
// screen), or Ctrl+Alt+L (manual sleep). Otherwise notes the activity.
bool power_mgmt_handle_key(uint8_t usage, uint8_t modifiers);

// Idle timeout in seconds; 0 = never sleep. Doesn't persist by itself.
void power_mgmt_set_timeout(uint16_t seconds);

// Force screen sleep immediately (console `sleep` command).
void power_mgmt_sleep_now(void);

// Lock the screen immediately (console `lock` command): same as Ctrl+Alt+L,
// only the hotkey unlocks.
void power_mgmt_lock_now(void);

// For /debug: is the backlight on, and seconds since the last activity.
bool power_mgmt_screen_on(void);
bool power_mgmt_locked(void);
uint32_t power_mgmt_idle_s(void);

// Hard power-off: pulses IO expander #2 (PI4IOE5V6408 @0x44) pin 4
// high->low a few times to trip the power latch. Does not return if the
// hardware cooperates.
void power_mgmt_poweroff(void);
