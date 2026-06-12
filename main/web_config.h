#pragma once
#include "settings.h"

// Starts the HTTP config server on port 80.
// GET  /             - config page
// GET  /api/targets  - JSON target list (passwords omitted)
// POST /api/targets  - replace target list (entries without "pass" keep the old one)
// POST /api/connect  - {"index": n} switch the active SSH target
// `on_update` runs after the target list changed (refresh panel etc.).
void web_config_start(settings_t *s, void (*on_update)(void));
