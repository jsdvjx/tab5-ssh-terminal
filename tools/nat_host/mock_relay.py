#!/usr/bin/env python3
"""
T5NAT/1 mock relay — a local reference/test backend for the Tab5 reverse
tunnel client (main/nat_tunnel.c). See docs/nat-protocol.md for the protocol.

What it does
------------
1. Serves a WebSocket at ws://0.0.0.0:<port>/nat (plain ws, no TLS — point the
   device's nat_url at it for testing).
2. On a new device WS: reads the first BINARY frame as HELLO, logs it, replies
   READY {"ok":true,"host":"test.local"}.
3. Self-test (default): immediately opens stream 1 to the device's port 80 and
   sends a small HTTP GET, then prints every DATA byte the device streams back
   — i.e. the device's own web-config page, proxied through the tunnel. Proves
   the full client path end to end, then CLOSEs the stream.
4. Public proxy (optional, --listen PORT): also runs a real TCP listener; each
   visitor connection is bridged to the device as its own stream, so you can
   `curl http://localhost:<listen>/` and reach the device's port 80.

Run
---
    python3 -m venv /tmp/natvenv && /tmp/natvenv/bin/pip install websockets
    /tmp/natvenv/bin/python tools/nat_host/mock_relay.py            # self-test only
    /tmp/natvenv/bin/python tools/nat_host/mock_relay.py --listen 8888

Point the device at it
----------------------
Set settings.nat_url to  ws://<this-mac-ip>:9999/nat  and nat_enabled=true
(see the temporary seed_defaults override used for the end-to-end test). The
device dials in; you see HELLO logged and the device's HTTP response printed.
"""
import argparse
import asyncio
import json
import re
import struct
import sys

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets  (in a venv, e.g. /tmp/natvenv)")

# ---- frame types (mirror docs/nat-protocol.md) ----------------------------
T_HELLO, T_READY = 0x01, 0x02
T_OPEN, T_DATA, T_CLOSE = 0x10, 0x11, 0x12
T_PING, T_PONG = 0x20, 0x21
NAMES = {0x01: "HELLO", 0x02: "READY", 0x10: "OPEN", 0x11: "DATA",
         0x12: "CLOSE", 0x20: "PING", 0x21: "PONG"}


def frame(ftype, sid, payload=b""):
    return bytes([ftype]) + struct.pack(">H", sid) + payload


def parse(msg):
    ftype = msg[0]
    sid = struct.unpack(">H", msg[1:3])[0] if len(msg) >= 3 else 0
    return ftype, sid, msg[3:]


class Device:
    """One connected device WS + its live streams (stream_id -> bytes Queue)."""
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 1
        self.bridges = {}     # sid -> asyncio.Queue; sink for device->relay DATA

    def new_stream_id(self):
        sid = self.next_id
        self.next_id = self.next_id + 1 if self.next_id < 0xFFFF else 1
        return sid

    async def send(self, ftype, sid, payload=b""):
        await self.ws.send(frame(ftype, sid, payload))


async def self_test(dev):
    """Open a stream to device:80, send an HTTP GET, print the response."""
    sid = dev.new_stream_id()
    q = asyncio.Queue()
    dev.bridges[sid] = q
    print(f"[selftest] OPEN stream {sid} -> device port 80")
    await dev.send(T_OPEN, sid, struct.pack(">H", 80))
    # tab5:test basic-auth; even a 401 proves the device served its own port 80.
    req = (b"GET / HTTP/1.0\r\nHost: test.local\r\n"
           b"Authorization: Basic dGFiNTp0ZXN0\r\n\r\n")
    print(f"[selftest] DATA stream {sid}: {req!r}")
    await dev.send(T_DATA, sid, req)

    chunks = []
    try:
        while True:
            item = await asyncio.wait_for(q.get(), timeout=8.0)
            if item is None:                     # CLOSE from device
                break
            chunks.append(item)
            blob = b"".join(chunks)
            # Stop once we have the full body (Content-Length satisfied) or, for
            # chunked/early data, a short grace period after the first bytes.
            m = re.search(rb"Content-Length:\s*(\d+)", blob, re.I)
            if m and b"\r\n\r\n" in blob:
                body = blob.split(b"\r\n\r\n", 1)[1]
                if len(body) >= int(m.group(1)):
                    break
    except asyncio.TimeoutError:
        print("[selftest] (no more data — device closed or HTTP/1.0 EOF)")

    dev.bridges.pop(sid, None)
    blob = b"".join(chunks)
    print(f"[selftest] <<< {len(blob)} bytes from device port 80 via stream {sid}:")
    print("-" * 64)
    print(blob.decode("utf-8", "replace"))
    print("-" * 64)
    await dev.send(T_CLOSE, sid)
    print(f"[selftest] CLOSE stream {sid} — round trip complete\n")


async def handle_device(ws, current):
    p = getattr(ws, "path", "/nat")
    if not str(p).endswith("/nat"):
        await ws.close()
        return
    print(f"[ws] device connected from {ws.remote_address}")
    dev = Device(ws)
    current["dev"] = dev
    did_selftest = False
    try:
        async for msg in ws:
            if isinstance(msg, str):
                continue                          # T5NAT is binary-only
            ftype, sid, payload = parse(msg)
            if ftype == T_HELLO:
                try:
                    hello = json.loads(payload.decode("utf-8"))
                except Exception:
                    hello = {}
                print(f"[ws] <<< HELLO {hello}")
                ready = {"ok": True, "host": "test.local"}
                await dev.send(T_READY, 0, json.dumps(ready).encode())
                print(f"[ws] >>> READY {ready}")
                if not did_selftest:
                    did_selftest = True
                    asyncio.ensure_future(self_test(dev))
            elif ftype == T_DATA:
                q = dev.bridges.get(sid)
                if q is not None:
                    await q.put(payload)
                else:
                    print(f"[ws] <<< DATA stream {sid}: {len(payload)} bytes (no bridge)")
            elif ftype == T_CLOSE:
                reason = payload.decode("utf-8", "replace") if payload else ""
                print(f"[ws] <<< CLOSE stream {sid} {reason}")
                q = dev.bridges.get(sid)
                if q is not None:
                    await q.put(None)
            elif ftype == T_PING:
                await dev.send(T_PONG, 0, payload)
                print("[ws] <<< PING  >>> PONG")
            elif ftype == T_PONG:
                pass
            else:
                print(f"[ws] <<< {NAMES.get(ftype, hex(ftype))} stream {sid}: "
                      f"{len(payload)} bytes")
    except websockets.ConnectionClosed:
        print("[ws] device disconnected")
    finally:
        for q in dev.bridges.values():
            try:
                q.put_nowait(None)
            except Exception:
                pass
        if current.get("dev") is dev:
            current["dev"] = None


async def run_public_listener(get_device, listen_port):
    async def on_visitor(reader, writer):
        dev = get_device()
        if dev is None:
            writer.close()
            return
        sid = dev.new_stream_id()
        q = asyncio.Queue()
        dev.bridges[sid] = q
        peer = writer.get_extra_info("peername")
        print(f"[proxy] visitor {peer} -> stream {sid}")
        await dev.send(T_OPEN, sid, struct.pack(">H", 80))

        async def dev_to_visitor():
            while True:
                item = await q.get()
                if item is None:
                    break
                writer.write(item)
                await writer.drain()
            try:
                writer.close()
            except Exception:
                pass

        async def visitor_to_dev():
            try:
                while True:
                    data = await reader.read(2048)
                    if not data:
                        break
                    await dev.send(T_DATA, sid, data)
            finally:
                await dev.send(T_CLOSE, sid)

        await asyncio.gather(dev_to_visitor(), visitor_to_dev(),
                             return_exceptions=True)
        dev.bridges.pop(sid, None)
        print(f"[proxy] visitor {peer} done (stream {sid})")

    await asyncio.start_server(on_visitor, "0.0.0.0", listen_port)
    print(f"[proxy] public TCP listener on 0.0.0.0:{listen_port} "
          f"(curl http://localhost:{listen_port}/ -> device port 80)")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9999, help="WS port (path /nat)")
    ap.add_argument("--listen", type=int, default=0,
                    help="also run a public TCP proxy on this port")
    args = ap.parse_args()

    current = {"dev": None}

    async def ws_handler(ws, *a):
        await handle_device(ws, current)

    await websockets.serve(ws_handler, "0.0.0.0", args.port)
    print(f"[relay] T5NAT mock listening on ws://0.0.0.0:{args.port}/nat")
    if args.listen:
        await run_public_listener(lambda: current["dev"], args.listen)
    await asyncio.Future()   # run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[relay] bye")
