// RX8130CE RTC @ I2C 0x32: system clock source at boot, kept in sync via
// SNTP once Wi-Fi is up.
#pragma once

#include <stdbool.h>
#include <time.h>

// Sets TZ (CST-8), probes the RTC and, if it holds a plausible time
// (year >= 2025), seeds the system clock from it. Returns true if the RTC
// answered on the bus.
bool rtc_rx8130_init(void);

// Raw access. Return false on I2C error (or RTC absent).
bool rtc_rx8130_read(struct tm *out);
bool rtc_rx8130_write(const struct tm *t);

// Start SNTP (call when Wi-Fi has an IP; safe to call repeatedly — only the
// first call does anything). On each sync the RTC is updated from system time.
void rtc_rx8130_sntp_start(void);
