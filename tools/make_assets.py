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


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "nerd"):
        build_nerd()
    if which in ("all", "cjk"):
        build_cjk_full()
    if which in ("all", "emoji"):
        build_emoji()
    print(f"\nasset pack ready: {OUT}\ncopy the 'tab5' folder to the SD card root")
