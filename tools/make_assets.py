#!/usr/bin/env python3
"""Build the Tab5 SD asset pack into assets_sd/tab5/.

Outputs:
  nerd24.bin    - Nerd Font symbols (powerline/devicons/material), LVGL binfont
  cjkfull24.bin - full CJK coverage @24px, LVGL binfont
  emoji24.bin   - color emoji atlas (Twemoji), 24x24 ARGB8888
                  format: "EMJ1" u32 count, count * { u32 codepoint, u32 offset },
                  blobs of 24*24*4 bytes at offset (relative to blob section start)

Copy the resulting `tab5/` folder to the SD card root.
Requires: node (npx lv_font_conv), python3 + Pillow, curl.
"""
import io
import os
import struct
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets_sd" / "tab5"
CACHE = ROOT / "tools" / ".cache"
OUT.mkdir(parents=True, exist_ok=True)
CACHE.mkdir(parents=True, exist_ok=True)

NERD_URL = "https://github.com/ryanoasis/nerd-fonts/releases/latest/download/NerdFontsSymbolsOnly.zip"
TWEMOJI_URL = "https://github.com/jdecked/twemoji/archive/refs/tags/v15.1.0.tar.gz"
ARIAL_UNICODE = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"


def fetch(url: str, dest: Path):
    if dest.exists() and dest.stat().st_size > 0:
        print(f"cached: {dest.name}")
        return
    print(f"downloading {url} ...")
    subprocess.run(["curl", "-fsSL", "-o", str(dest), url], check=True)


def lv_font_conv(args):
    subprocess.run(["npx", "--yes", "lv_font_conv@1.5.3"] + args, check=True)


def build_nerd():
    out = OUT / "nerd24.bin"
    z = CACHE / "nerd.zip"
    fetch(NERD_URL, z)
    ttf = CACHE / "SymbolsNerdFontMono-Regular.ttf"
    if not ttf.exists():
        with zipfile.ZipFile(z) as f:
            for n in f.namelist():
                if n.endswith("SymbolsNerdFontMono-Regular.ttf"):
                    ttf.write_bytes(f.read(n))
                    break
    lv_font_conv([
        "--font", str(ttf), "--size", "20", "--bpp", "2", "--format", "bin",
        "-r", "0x23FB-0x23FE", "-r", "0x2665", "-r", "0x26A1", "-r", "0x2B58",
        "-r", "0xE000-0xF8FF",          # PUA: powerline, devicons, font-awesome...
        "-r", "0xF0001-0xF1AF0",        # material design icons (supplementary PUA)
        "--no-compress", "-o", str(out),
    ])
    print(f"nerd24.bin: {out.stat().st_size // 1024} KB")


def build_cjk_full():
    out = OUT / "cjkfull24.bin"
    lv_font_conv([
        "--font", ARIAL_UNICODE, "--size", "24", "--bpp", "2", "--format", "bin",
        "-r", "0x2E80-0x303F",          # radicals, CJK punctuation
        "-r", "0x3041-0x33FF",          # kana, CJK symbols
        "-r", "0x4E00-0x9FFF",          # unified
        "-r", "0xF900-0xFAFF",          # compat
        "-r", "0xFE30-0xFE4F",
        "-r", "0xFF00-0xFFE6",          # fullwidth
        "-r", "0x2014-0x2026",
        "--no-compress", "-o", str(out),
    ])
    print(f"cjkfull24.bin: {out.stat().st_size // 1024} KB")


def build_emoji():
    from PIL import Image

    out = OUT / "emoji24.bin"
    tgz = CACHE / "twemoji.tar.gz"
    fetch(TWEMOJI_URL, tgz)

    entries = []   # (codepoint, argb_bytes)
    with tarfile.open(tgz) as tf:
        for m in tf.getmembers():
            if "/assets/72x72/" not in m.name or not m.name.endswith(".png"):
                continue
            stem = Path(m.name).stem
            parts = stem.split("-")
            # single-codepoint emoji only (optionally with FE0F variant tail)
            if len(parts) == 2 and parts[1] == "fe0f":
                parts = parts[:1]
            if len(parts) != 1:
                continue
            try:
                cp = int(parts[0], 16)
            except ValueError:
                continue
            data = tf.extractfile(m).read()
            img = Image.open(io.BytesIO(data)).convert("RGBA").resize(
                (24, 24), Image.LANCZOS)
            raw = bytearray()
            for px in img.getdata():
                r, g, b, a = px
                raw += struct.pack("<BBBB", b, g, r, a)   # LVGL ARGB8888 LE
            entries.append((cp, bytes(raw)))

    entries.sort(key=lambda e: e[0])
    with open(out, "wb") as f:
        f.write(b"EMJ1")
        f.write(struct.pack("<I", len(entries)))
        off = 0
        for cp, _ in entries:
            f.write(struct.pack("<II", cp, off))
            off += 24 * 24 * 4
        for _, blob in entries:
            f.write(blob)
    print(f"emoji24.bin: {len(entries)} emoji, {out.stat().st_size // 1024} KB")


# OS / brand icon atlas. IDs MUST match settings.h target_os_t:
#   0 SERVER (generic), 1 APPLE, 2 TUX(linux), 3 UBUNTU, 4 DEBIAN,
#   5 WINDOWS, 6 RASPBERRY.
# Colored Devicon "-original" SVG variants (MIT). Server (0) has no Devicon
# logo, so we synthesize a flat 2-tone slate/teal rack glyph inline.
DEVICON_RAW = "https://raw.githubusercontent.com/devicons/devicon/master/icons"
OSICON_SOURCES = {
    1: f"{DEVICON_RAW}/apple/apple-original.svg",
    2: f"{DEVICON_RAW}/linux/linux-original.svg",
    3: f"{DEVICON_RAW}/ubuntu/ubuntu-original.svg",
    4: f"{DEVICON_RAW}/debian/debian-original.svg",
    5: f"{DEVICON_RAW}/windows11/windows11-original.svg",
    6: f"{DEVICON_RAW}/raspberrypi/raspberrypi-original.svg",
}

# Inline neutral colored server-rack glyph for id 0 (slate body, teal LEDs).
SERVER_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <rect x="20" y="16" width="88" height="32" rx="6" fill="#475569"/>
  <rect x="20" y="56" width="88" height="32" rx="6" fill="#475569"/>
  <rect x="20" y="96" width="88" height="20" rx="6" fill="#334155"/>
  <circle cx="34" cy="32" r="5" fill="#2dd4bf"/>
  <circle cx="50" cy="32" r="5" fill="#2dd4bf"/>
  <circle cx="34" cy="72" r="5" fill="#2dd4bf"/>
  <circle cx="50" cy="72" r="5" fill="#2dd4bf"/>
  <rect x="74" y="27" width="24" height="10" rx="2" fill="#94a3b8"/>
  <rect x="74" y="67" width="24" height="10" rx="2" fill="#94a3b8"/>
</svg>
"""


def build_osicons():
    from PIL import Image

    dcache = CACHE / "devicon"
    dcache.mkdir(parents=True, exist_ok=True)

    # Collect SVG paths per id (download Devicon, write inline server glyph).
    svgs = {}  # id -> Path
    server_svg = dcache / "server-generic.svg"
    server_svg.write_text(SERVER_SVG)
    svgs[0] = server_svg
    for oid, url in OSICON_SOURCES.items():
        name = url.rsplit("/", 1)[-1]
        dest = dcache / name
        fetch(url, dest)
        svgs[oid] = dest

    ids = sorted(svgs)

    def build_one(size):
        out = OUT / f"osicons{size}.bin"
        entries = []  # (id, argb_bytes)
        for oid in ids:
            png = dcache / f"{svgs[oid].stem}-{size}.png"
            subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size),
                            str(svgs[oid]), "-o", str(png)], check=True)
            img = Image.open(png).convert("RGBA")
            if img.size != (size, size):
                img = img.resize((size, size), Image.LANCZOS)
            raw = bytearray()
            for r, g, b, a in img.getdata():
                raw += struct.pack("<BBBB", b, g, r, a)  # LVGL ARGB8888 LE, straight alpha
            entries.append((oid, bytes(raw)))
        blob = size * size * 4
        with open(out, "wb") as f:
            f.write(b"OSI1")
            f.write(struct.pack("<I", len(entries)))
            off = 0
            for oid, _ in entries:
                f.write(struct.pack("<II", oid, off))
                off += blob
            for _, b in entries:
                f.write(b)
        print(f"osicons{size}.bin: {len(entries)} icons, {out.stat().st_size // 1024} KB")

    build_one(64)
    build_one(32)


def build_pinyin_dict():
    """dict_pinyin.dat for the ime_pinyin component (v2 uint32 format).

    Rebuilds from the raw dictionary via the ime_host dictbuilder (which
    compiles the components/ime_pinyin sources) for reproducibility.
    """
    out = OUT / "dict_pinyin.dat"
    ime_host = ROOT / "tools" / "ime_host"
    subprocess.run(["make", "build/dict_pinyin.dat"], cwd=ime_host, check=True)
    out.write_bytes((ime_host / "build" / "dict_pinyin.dat").read_bytes())
    print(f"dict_pinyin.dat: {out.stat().st_size // 1024} KB (v2 uint32 format)")


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "nerd"):
        build_nerd()
    if which in ("all", "cjk"):
        build_cjk_full()
    if which in ("all", "emoji"):
        build_emoji()
    if which in ("all", "osicons"):
        build_osicons()
    if which in ("all", "pinyin"):
        build_pinyin_dict()
    print(f"\nasset pack ready: {OUT}\ncopy the 'tab5' folder to the SD card root")
