#pragma once

// M5Stack "Keyboard for Tab5" (70-key, STM32F030) on Ext.Port1:
// I2C addr 0x6D, SDA=G0, SCL=G1, INT=G50. Runs the keyboard in HID mode and
// feeds (modifier, keycode) events through hid_send_key(), same path as a
// USB keyboard. Safe to call when no keyboard is attached (probes first).
void i2c_keyboard_start(void);
