#!/usr/bin/env python3
# Boot animation design preview. Renders the concept as an animated GIF so it
# can be reviewed before implementing the same sequence procedurally in LVGL
# (firmware: main/boot_anim.c). Timeline (60 ticks = ~3.0s at 50ms/frame):
#   0.0-0.4s  blinking block cursor alone
#   0.4-1.2s  "tab5" typed one char per ~0.2s, cursor rides along
#   1.2-1.6s  cyan powerline sweep underline, left to right
#   1.6-2.2s  "SSH TERMINAL" subtitle fades in below
#   2.2-3.0s  hold, cursor keeps blinking
from PIL import Image, ImageDraw, ImageFont

W, H = 640, 360               # preview at half device res (device: 1280x720)
S = W / 1280.0                # scale factor vs device
BG = (13, 13, 13)
FG = (220, 220, 220)
CYAN = (62, 199, 192)
GREY = (120, 120, 120)

big = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(150 * S))
sub = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", int(34 * S))

def text_w(d, t, f):
    b = d.textbbox((0, 0), t, font=f)
    return b[2] - b[0], b[3] - b[1]

frames = []
TOTAL = 60
for i in range(TOTAL):
    t = i * 0.05
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)

    full = "tab5"
    n = 0 if t < 0.4 else min(len(full), int((t - 0.4) / 0.2) + 1)
    typed = full[:n]

    fw, fh = text_w(d, full, big)
    cw, _ = text_w(d, "a", big)
    x0 = (W - (fw + cw + int(10 * S))) // 2
    y0 = int(H * 0.34) - fh // 2

    if typed:
        d.text((x0, y0 - int(28 * S)), typed, font=big, fill=FG)
    tw = text_w(d, typed, big)[0] if typed else 0

    blink = (i % 12) < 7
    if blink:
        cx = x0 + tw + int(10 * S)
        d.rectangle([cx, y0 - int(20 * S), cx + cw, y0 + fh - int(4 * S)],
                    fill=CYAN if n < len(full) else FG)

    if t >= 1.2:                                    # powerline sweep
        prog = min(1.0, (t - 1.2) / 0.4)
        ly = y0 + fh + int(26 * S)
        lw = int((fw + cw + int(10 * S)) * prog)
        d.rectangle([x0, ly, x0 + lw, ly + int(6 * S)], fill=CYAN)
        ax = x0 + lw                                # powerline arrow head
        d.polygon([(ax, ly - int(6 * S)), (ax + int(14 * S), ly + int(3 * S)),
                   (ax, ly + int(12 * S))], fill=CYAN)

    if t >= 1.6:                                    # subtitle fade
        a = min(1.0, (t - 1.6) / 0.6)
        col = tuple(int(BG[k] + (GREY[k] - BG[k]) * a) for k in range(3))
        st = "S S H   T E R M I N A L"
        sw, _ = text_w(d, st, sub)
        d.text(((W - sw) // 2, y0 + fh + int(56 * S)), st, font=sub, fill=col)

    frames.append(im)

frames[0].save("/tmp/bootanim_preview.gif", save_all=True,
               append_images=frames[1:], duration=50, loop=0)
print("wrote /tmp/bootanim_preview.gif")
