# Tab5 SSH Terminal

**English** | [中文](README.zh.md)

A hardware SSH terminal for the **M5Stack Tab5** (ESP32-P4): full xterm-256color
emulation with CJK / Nerd Font / color emoji, a touch launcher UI, an on-device
pinyin IME, BLE provisioning, multi-session SSH and key auth — built on ESP-IDF.

Turn the Tab5 into a pocket SSH terminal / handheld vibecoding device: SSH into a
dev box to run Claude Code, tmux and vim, with a physical keyboard, a touchscreen
and a full terminal experience (truecolor, CJK, Nerd Font, color emoji).

> repo: <https://github.com/jsdvjx/tab5-ssh-terminal>
> BLE setup page: <https://t5.cc.hn>

---

## Features

### Terminal
- Custom xterm-256color core (`main/term/`): 256-color / truecolor, scroll
  regions, alternate screen, wide chars (CJK = 2 cells), bold / underline /
  reverse — Claude Code, tmux, htop and vim all work
- LVGL canvas renderer: dirty-row tracking, font fallback chain, powerline
  separators drawn as geometry, em-wide icons / emoji in a second overflow pass
- SD asset pack: full CJK font + Nerd Font + ~1500 color Twemoji + a color
  OS-logo atlas, loaded into 32 MB PSRAM at boot; built-in fallback font if no card

### SSH
- libssh2 (mbedTLS): **up to 4 concurrent sessions**, `Ctrl+Alt+1..9` to
  switch / lazy-open, `Ctrl+Alt+W` to close
- **Per-target auth**: auto (key first) / password-only / certificate-only
- **Device key**: auto-generates RSA-2048 at first boot (stored in NVS),
  `sshkey` shows the public line, custom private keys can be uploaded over BLE
- **Host-key pinning**: pins the fingerprint on first connect; a mismatch aborts
  with a loud red warning
- Auto-reconnect on drop

### GUI
- Launcher home: tap a colorful card to expand it into an app + a bottom dock,
  with color brand OS icons
- SSH device grid: big OS logo (Apple/Linux/Ubuntu/Debian/Windows/Raspberry/
  server) + status dot; tapping a device shows a Connect / Edit / Cancel sheet
- Graphical SSH target CRUD (on-screen keyboard, auto-hidden when a hardware
  keyboard is attached)
- Top bar: Wi-Fi signal icon + IP + battery + clock, with a connecting animation
- Boot animation (typewriter logo) held as a splash until the panel is ready
- **Multilingual**: English / 中文, switchable at runtime (`main/i18n.*`)

### Connectivity / input / power
- **Wi-Fi**: multiple saved credentials, scan-to-pick, background infinite-retry
  connect
- **BLE provisioning** (NimBLE, HCI over SDIO): push Wi-Fi creds + an SSH private
  key from a phone / Chrome via <https://t5.cc.hn>; off by default, toggled in the
  panel
- **Pinyin IME**: a port of libgooglepinyin (`components/ime_pinyin`),
  `Ctrl+Space` to toggle, an LVGL candidate bar, dictionary on SD — type Chinese
  into any remote program
- **Local shell** (esp_console): `Ctrl+Alt+T` to toggle; `free` / `ps` / `batt`
  / `ls` / `sshkey` / `ble` … a full on-device console
- **Power**: INA226 gauge, RX8130 RTC, CPU temp; idle screen-sleep (any key /
  touch wakes), `Ctrl+Alt+L` lock (anti-mistouch, only `Ctrl+Alt+L` unlocks),
  soft power-off, automatic charge-enable
- Dual keyboards: USB-A physical keyboard + the M5Stack Tab5 keyboard (I2C HID),
  sharing one keycode pipeline

---

## Hardware

M5Stack Tab5: ESP32-P4 (dual-core RISC-V 360 MHz, 32 MB PSRAM, 16 MB flash) +
ESP32-C6 (Wi-Fi / BLE over SDIO), a 5″ 1280×720 MIPI-DSI touchscreen, and a
removable NP-F550 battery (2S 7.4 V 2000 mAh).

---

## Build

```sh
# ESP-IDF v5.5.x
. ~/esp/esp-idf/export.sh
idf.py build flash monitor
```

The device boots straight into the GUI. Set Wi-Fi credentials via
<https://t5.cc.hn> (BLE), the panel's *Connectivity → Add Network*, or the web
config page; then add a target host in the SSH card.

### SD asset pack (strongly recommended)

```sh
python3 tools/make_assets.py        # needs node, python3 + Pillow, rsvg-convert
# Option A: copy assets_sd/tab5/ to the SD card root
# Option B: no card reader — push over HTTP (the device formats a blank card):
for f in assets_sd/tab5/*.bin; do
  curl -X POST --data-binary @"$f" "http://<device-ip>/api/sdput?name=$(basename "$f")"
done
curl -X POST http://<device-ip>/api/reboot
```

Pack contents: `nerd24/cjkfull24/emoji24` (terminal fonts + emoji),
`osicons32/64` (color OS logos), `dict_pinyin.dat` (pinyin dictionary).

---

## Remote debugging / Web API

Read-only debug infrastructure kept from development (state-changing control
endpoints were removed):

| Endpoint | Purpose |
|---|---|
| `GET /` | SSH target config page |
| `GET/POST /api/targets` | Read/write the target list (passwords not echoed; blank keeps the old one) |
| `POST /api/sdput?name=x` | Write a file to the SD card under `/tab5/` |
| `POST /api/reboot` | Reboot |
| `GET /shot[?full=1&panel=1&…]` | BMP screenshot (`full=1` whole screen incl. panel; `&edit/&conn/&keys/&sheet/&lang` are UI-dev helpers) |
| `GET /debug` | Input / SSH / render pipeline counters + heap + battery |
| `GET /api/ime?py=nihao` | Pinyin engine query (verify without a keyboard) |

---

## Layout

```
main/
  term/                terminal core + LVGL render (dirty rows / font fallback / powerline geometry)
  ssh_client.c         multi-session libssh2, per-target auth, host-key pinning
  ssh_keys.c           device RSA key (NVS / background gen), pinned-fingerprint store
  hid_keyboard.c       USB HID keyboard + keycode translation (shared by both keyboards)
  i2c_keyboard.c       Tab5 keyboard (I2C HID mode)
  ui_home.c            launcher panel: cards / dock / device grid / apps / edit form
  i18n.c               multilingual string table (en / zh, runtime-switchable)
  ime_filter.cpp       pinyin IME state machine (keyboard filter → ssh_client_send)
  ime_bar.c            LVGL candidate bar
  local_shell.c        esp_console on-device shell (Ctrl+Alt+T)
  ble_prov.c           NimBLE provisioning + key upload (HCI over SDIO)
  wifi.c               C6 power-up + esp_hosted late init + scan / connect / multi-cred
  power_mon.c          INA226 voltage / current / power
  power_mgmt.c         screen-sleep / lock / power-off / charge-enable
  rtc_rx8130.c         RX8130 RTC + SNTP
  status_bar.c         top status bar
  boot_anim.c          boot animation / splash
  assets.c             SD asset loading (binfont / emoji atlas / OS icons / dict)
  settings.c           NVS config (multi Wi-Fi / SSH targets / preferences)
  web_config.c         HTTP config server + debug endpoints
components/
  ime_pinyin/          libgooglepinyin port (ESP-IDF component, Apache-2.0)
  m5stack_tab5/        localized BSP (ST7121 panel support, board-rev-3 auto-detect)
  esp_lcd_st7121/      ST7121 driver (ported from M5Tab5-UserDemo)
tools/
  make_assets.py       SD asset pack generator (Nerd / CJK / Twemoji / color logos / dict)
  bootanim_preview.py  boot-animation design preview (renders a GIF)
  patch_esp_hosted.py  required esp_hosted patch (auto-runs at CMake configure)
  ime_host/            Mac-side libgooglepinyin validation (CLI test; clone not committed)
web/
  t5_site/             BLE setup page (deployed at t5.cc.hn; not committed)
```

---

## Gotchas (read before swapping boards / bumping deps)

1. **ST7121 panel**: units made after 2026-04-28 use an ST7121; esp-bsp ≤1.2.0
   only knows ST7123 and misdetects, so init writes are silently ignored → black
   screen with working touch. The local BSP auto-detects the board revision from
   the touch firmware version (reg 0x0000: 1 = ST7121, 3 = ST7123).
2. **esp_hosted constructor boot loop**: its `__attribute__((constructor))`
   inits before the heap is ready, so the SDIO mempool alloc fails.
   `tools/patch_esp_hosted.py` re-applies the fix at build time (lost when deps
   are re-fetched — don't delete the script).
3. **PSRAM-stack tasks must not touch flash**: session task stacks live in PSRAM
   to save internal RAM, but any NVS / flash op there asserts after cache is
   disabled (boot loop). Host-key pins use a RAM cache + an internal-stack worker
   for persistence.
4. **Internal RAM pressure**: with many LVGL objects,
   `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` was lowered from 4096 to 256 so small
   allocations go to PSRAM — otherwise the SSH handshake runs out of internal RAM.
5. **C6 firmware 0.0.0**: the stock C6 esp_hosted firmware is old and sometimes
   RPC-times-out before GOT_IP; boot uses background infinite retry to absorb it
   without blocking the UI.
6. **New bool settings**: an appended field reads zero on old blobs (= false), so
   default-on flags must be designed so 0 = the desired default (e.g. `ble_enabled`
   defaults off).
7. **LVGL rendering**: a draw task for a missing glyph deadlocks the canvas
   (guarded with a `get_glyph_dsc` precheck); SD-loaded big fonts need
   `CONFIG_LV_USE_CLIB_MALLOC=y`; binfonts with >1 MB of glyphs need
   `CONFIG_LV_FONT_FMT_TXT_LARGE=y` (else the 20-bit bitmap_index overflows → garbage).
8. The SD card and the C6 share the SDMMC peripheral: assets are read and the
   card unmounted before Wi-Fi starts.

---

## License

MIT. `components/ime_pinyin` is ported from libgooglepinyin (Apache-2.0);
`components/esp_lcd_st7121` is from M5Stack; the color OS icons are from
[Devicon](https://github.com/devicons/devicon) (MIT). Each dependency keeps its
original license.
