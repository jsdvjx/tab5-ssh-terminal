// Top-of-screen status bar: SSH state, IME mode, clock, Wi-Fi, CPU temp,
// battery. TAB5_STATUS_BAR_H pixels tall (term.h); the terminal canvas and
// side panel sit below it.
#pragma once

#include <stdbool.h>

// Create the bar and start its update timer. Call after term_render_start()
// (display up, LVGL running).
void status_bar_init(void);

// IME mode indicator ("EN", later "中"). Safe from any task.
void status_bar_set_ime(const char *txt);

// Voice recording indicator ("REC" while capturing, "..." while
// transcribing, "ERR" flash on failure). NULL hides it. Safe from any task.
void status_bar_set_rec(const char *txt);

// Latest chip temperature in °C (console `temp` command). Returns false if
// the internal sensor is unavailable.
bool status_bar_get_temp(float *celsius);
