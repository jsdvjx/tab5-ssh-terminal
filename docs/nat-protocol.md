# T5NAT/1 — Tab5 reverse-tunnel ("NAT") wire protocol

A Tab5 device dials a single **secure WebSocket** to a relay and exposes its
own local TCP ports (e.g. `80` = the device web-config server) at a public
subdomain. Public visitors hit `https://<sub>.t5.cc.hn/`, the relay forwards
the raw TCP stream over the WebSocket, the device proxies it to
`127.0.0.1:<port>` and streams the answer back. No inbound port-forwarding on
the device's LAN is needed — the device is always the dialer.

```
   visitor ──TCP──▶ RELAY ──WSS(T5NAT/1)──▶ Tab5 ──TCP──▶ 127.0.0.1:80 (web_config)
```

This document is the contract a backend MUST implement to interoperate with
the device client (`main/nat_tunnel.c`). The reference mock relay
(`tools/nat_host/mock_relay.py`) implements exactly this and is enough to
test the device end-to-end without the real backend.

## Transport

* One WebSocket connection per device to `wss://t5.cc.hn/nat` (configurable;
  the path is `/nat`). `ws://` is accepted by the client too (used for the
  local mock; production is `wss://`).
* The device sends the standard WebSocket sub-protocol header
  `Sec-WebSocket-Protocol: t5nat1`. The relay SHOULD echo it but the client
  does not require it.
* **Every** application frame is a WebSocket **BINARY** message. Text frames
  are ignored. WebSocket PING/PONG (control opcodes) are handled by the WS
  layer; T5NAT also has its own app-level PING/PONG (below) for end-to-end
  liveness across proxies that swallow WS control frames.

## Frame format

Every binary message is one frame:

```
 byte 0      : u8   type
 bytes 1..2  : u16  stream_id   (big-endian / network order)
 bytes 3..   : payload          (type-specific; may be empty)
```

`stream_id == 0` is the **control** stream (HELLO / READY / PING / PONG).
Data streams use relay-assigned non-zero ids (1..65535).

### Types

| type   | name   | dir            | payload |
|--------|--------|----------------|---------|
| `0x01` | HELLO  | client → relay | JSON (see below) |
| `0x02` | READY  | relay → client | JSON (see below) |
| `0x10` | OPEN   | relay → client | `[u16 port BE]` — open a stream to this local port |
| `0x11` | DATA   | both           | raw TCP bytes for `stream_id` |
| `0x12` | CLOSE  | both           | optional UTF-8 reason |
| `0x20` | PING   | both (stream 0)| optional opaque bytes |
| `0x21` | PONG   | both (stream 0)| echo of the PING payload |

### `0x01 HELLO` (client → relay), stream 0

Sent immediately after the WebSocket opens. Payload is JSON:

```json
{ "v": 1, "token": "<device token>", "sub": "<desired subdomain>", "ports": [80, 8080] }
```

* `v` — protocol version, always `1`.
* `token` — device auth token (opaque to the protocol; the relay maps it to an
  account / allowed subdomains). May be empty in the mock.
* `sub` — desired subdomain label (e.g. `"alice"` → `alice.t5.cc.hn`). May be
  empty to let the relay assign one.
* `ports` — the local TCP ports the device is willing to expose. The relay may
  only ever `OPEN` one of these; the client **rejects** an OPEN for any port
  not in this list (replies `CLOSE`). Default `[80]` (the web-config server).

### `0x02 READY` (relay → client), stream 0

Sent once after the relay has accepted (or rejected) the HELLO:

```json
{ "ok": true,  "host": "alice.t5.cc.hn" }
```
or
```json
{ "ok": false, "err": "subdomain taken" }
```

On `ok:true` the client marks itself connected and exposes `host` in the UI /
`nat_tunnel_status()`. On `ok:false` the client logs the error and closes the
WebSocket; auto-reconnect/backoff then retries.

### `0x10 OPEN` (relay → client)

A new public visitor connected. `stream_id` is the **relay-assigned** id for
this stream (must be non-zero and not currently in use). Payload:

```
[u16 port BE]   — which of the device's exposed local ports to connect to
```

The client:
1. Validates `port` is in its exposed `ports` list. If not → `CLOSE(stream_id)`.
2. Validates it has a free stream slot (`MAX_STREAMS`, default 8). If not →
   `CLOSE(stream_id)`.
3. Opens a TCP socket to `127.0.0.1:port` (loopback). On connect failure →
   `CLOSE(stream_id)`.
4. Maps `stream_id ↔ socket` and starts pumping.

There is no explicit OPEN-ack: the first `DATA` (or a `CLOSE`) from the client
is the implicit result.

### `0x11 DATA` (both directions)

Opaque TCP payload for `stream_id`.

* relay → client: bytes the visitor sent — the client `write()`s them to the
  local socket.
* client → relay: bytes the local service produced — the relay forwards them
  to the visitor.

The client fragments large local reads into DATA frames of at most
`NAT_DATA_CHUNK` (default 1460) bytes. There is no per-frame length field
beyond the WebSocket frame length itself.

### `0x12 CLOSE` (both directions)

Tear down `stream_id`. Optional UTF-8 payload = human reason (logged, not
parsed). After sending or receiving CLOSE for a stream, both sides free it; a
later DATA for a freed stream is dropped (and may trigger a defensive CLOSE).

* Local socket EOF/error → client sends `CLOSE(stream_id)` and frees the slot.
* Relay→client `CLOSE` → client closes the local socket and frees the slot.

### `0x20 PING` / `0x21 PONG` (control, stream 0)

App-level keepalive in addition to WebSocket-level ping. Either side may send
`PING` with an opaque payload; the peer replies `PONG` echoing it. The client
sends a `PING` every `NAT_PING_INTERVAL_S` (default 25 s) of idle and treats
the connection as dead if no PONG (or any frame) arrives within
`NAT_PING_TIMEOUT_S` (default 60 s), forcing a reconnect.

## Client lifecycle / reconnect

```
connect WSS → send HELLO → wait READY
   READY ok    → CONNECTED, serve OPEN/DATA/CLOSE, periodic PING
   READY !ok   → close, backoff, retry
   WSS drop / PONG timeout → close all streams, backoff, retry
```

Reconnect backoff is exponential: `2s, 4s, 8s … capped at 30s`, reset to 2s
on a successful READY.

## Limits the client enforces

| constant            | default | meaning |
|---------------------|---------|---------|
| `MAX_STREAMS`       | 8       | concurrent public connections |
| `NAT_DATA_CHUNK`    | 1460    | max bytes per outgoing DATA frame |
| `NAT_PING_INTERVAL_S` | 25    | idle before client sends app PING |
| `NAT_PING_TIMEOUT_S`  | 60    | no traffic → reconnect |
| `NAT_RX_FRAME_MAX`  | 8 KB    | max single inbound WS message payload |

## What a backend MUST implement

To match this client, the relay must:

1. Accept a WebSocket at `…/nat`, read the first BINARY frame as `HELLO`,
   allocate/verify the subdomain from `token`+`sub`, reply `READY`.
2. Run a public TCP/HTTP front-end per device; on each new visitor connection
   allocate a fresh non-zero `stream_id`, send `OPEN(stream_id, port)` (with a
   port from the device's `ports` list), then relay bytes both ways as `DATA`
   frames and tear down with `CLOSE`.
3. Honour `CLOSE` from the device, answer `PING` with `PONG`, and never reuse a
   `stream_id` that is still open.

That is the entire contract — see `tools/nat_host/mock_relay.py` for a working
~250-line reference and `main/nat_tunnel.c` for the device side.
