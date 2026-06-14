// T5NAT/1 reverse-tunnel client: dials a single secure WebSocket to a relay
// and exposes the device's own local TCP ports (e.g. 80 = web_config) at a
// public subdomain. See docs/nat-protocol.md for the wire protocol.
//
// All functions are safe to call from any task. The tunnel runs on its own
// FreeRTOS task plus the esp_websocket_client transport task; init/start/stop
// only post to it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "settings.h"

// Capture the settings pointer (nat_url / nat_token / nat_sub / nat_enabled).
// `s` must outlive the tunnel (app_main's static settings). Call after wifi.
void nat_tunnel_init(settings_t *s);

// Start dialing the relay (idempotent). Spawns the pump task on first call;
// later calls just re-arm a stopped tunnel. Honour nat_enabled at the call
// site — this starts unconditionally once invoked.
void nat_tunnel_start(void);

// Stop the tunnel: closes the WebSocket and all streams, the pump task idles.
// A later nat_tunnel_start() resumes. Idempotent.
void nat_tunnel_stop(void);

// True if currently connected (READY ok received). When connected and `host`
// is non-NULL, copies the assigned public host (e.g. "alice.t5.cc.hn") into
// it. Returns false (and leaves host empty) while connecting/stopped.
bool nat_tunnel_status(char *host, size_t cap);
