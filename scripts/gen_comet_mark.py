#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the Comet mark: a comet whose tail is a mesh.

The head is a node sending out radio waves; the tail is a band of mesh nodes and
links streaming away from it, thinning out the further they are from the head.
One radio, every mesh -- in one picture.

One geometry, every output:
- src/ui-touch/CometMarkGeom.h: nodes, links, the hop path and the radio arcs, which
  ui-touch/CometMark.cpp draws (and animates) at any size, in ink on e-paper and in
  light on colour panels.
- src/comet_mark_rgb.h: the still mark as an anti-aliased white-on-black RGB565 bitmap
  (208 x 110), for the screens painted before LVGL runs (main.cpp's boot screen, the
  remote-mode placeholder). E-paper boards invert it.
- assets/comet-mark.svg and assets/favicon.ico: the same mark for the web build and
  the repository.
- src/ui-touch/assets/lockscreen_placeholder_jpg.h: the stock lock wallpaper (320 x 240
  JPEG: the mark and the wordmark on a dark green ground), seeded for the picture
  picker and the fallback when a chosen wallpaper cannot be read.

Run:  python3 scripts/gen_comet_mark.py   (needs Pillow)
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

W, H = 208, 110          # the box (drawing units = pixels at scale 1)
HX, HY = 168, 72         # the head
AXIS = 195.0             # tail direction, degrees (0 = right, y grows down: 195 = left, a little up)
LENGTH = 154.0           # tail length
BEND = -12.0             # the tail curves away (a dust tail bends), px at its end
ROWS = [(0.11, 1), (0.22, 2), (0.35, 2), (0.50, 3), (0.66, 3), (0.83, 4), (1.00, 3)]   # (along, nodes)
DUST = 9                 # loose motes beside the band: no links
ARCS = [(13, 2.2, 0.95), (21, 2.0, 0.65), (29, 1.8, 0.38)]   # radio waves: radius, width, light
ARC_HALF = 38.0          # half the opening angle of a wave, degrees
SS = 6                   # supersampling for the bitmap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def along(s, off):
    """A point s (0 head .. 1 end) along the tail's curved centre line, off px across it."""
    a = math.radians(AXIS)
    dx, dy = math.cos(a), math.sin(a)
    nx, ny = -dy, dx                       # the normal (points "up" for this axis)
    bend = BEND * s * s
    return (HX + dx * LENGTH * s + nx * (bend + off), HY + dy * LENGTH * s + ny * (bend + off))


def geometry():
    rnd = random.Random(7)
    nodes = []           # (x, y, r, row, dist)
    rows = []
    for ri, (s, count) in enumerate(ROWS):
        half = 3.0 + 24.0 * s ** 1.15       # the band widens away from the head
        row = []
        for k in range(count):
            off = 0.0 if count == 1 else -half + 2 * half * k / (count - 1)
            off += rnd.uniform(-0.18, 0.18) * half / max(1, count - 1) * 2
            ss = s + rnd.uniform(-0.025, 0.025)
            x, y = along(ss, off)
            dist = math.hypot(x - HX, y - HY)
            r = 0.85 + 1.9 * (1.0 - dist / 165.0)
            row.append(len(nodes))
            nodes.append((x, y, r, ri, dist))
        rows.append(row)
    links = set()
    for ri in range(1, len(rows)):
        for n in rows[ri]:
            prev = sorted(rows[ri - 1], key=lambda m: math.dist(nodes[n][:2], nodes[m][:2]))
            for m in prev[:2]:
                links.add(tuple(sorted((n, m))))
        for k in range(len(rows[ri]) - 1):
            links.add((rows[ri][k], rows[ri][k + 1]))
    head_links = list(rows[0])
    # A message hopping in to the head: from the middle of the last row, always to the
    # linked node one row nearer the head that is closest to the centre line.
    path = [rows[-1][len(rows[-1]) // 2]]
    for ri in range(len(rows) - 2, -1, -1):
        cur = path[-1]
        cands = [m for m in rows[ri] if tuple(sorted((cur, m))) in links]
        path.append(min(cands, key=lambda m: abs(nodes[m][1] - along(ROWS[ri][0], 0)[1])))
    dust = []
    for _ in range(DUST):
        s = rnd.uniform(0.3, 1.05)
        half = 3.0 + 24.0 * s ** 1.15
        off = rnd.choice((-1, 1)) * half * rnd.uniform(1.2, 1.7)
        x, y = along(s, off)
        if 2 < x < W - 2 and 2 < y < H - 2:
            dust.append((x, y, rnd.uniform(0.5, 0.9), math.hypot(x - HX, y - HY)))
    return nodes, sorted(links), head_links, path, dust


def light(d):
    """How bright a part of the tail is, from its distance to the head (1 near, ~0.25 far)."""
    return 0.25 + 0.75 * max(0.0, 1.0 - d / 160.0) ** 1.3


def render_bitmap(nodes, links, head_links, dust):
    img = Image.new("L", (W * SS, H * SS), 0)
    d = ImageDraw.Draw(img)

    def P(x, y):
        return (x * SS, y * SS)

    for a, b in links:
        na, nb = nodes[a], nodes[b]
        v = int(255 * 0.55 * light((na[4] + nb[4]) / 2))
        d.line([P(na[0], na[1]), P(nb[0], nb[1])], fill=v, width=int(1.1 * SS))
    for n in head_links:
        nn = nodes[n]
        d.line([P(HX, HY), P(nn[0], nn[1])], fill=int(255 * 0.7), width=int(1.3 * SS))
    for x, y, r, _row, dist in nodes:
        v = int(255 * min(1.0, 0.35 + light(dist)))
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=v)
    for x, y, r, dist in dust:
        v = int(255 * 0.8 * light(dist))
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=v)
    # radio waves to the right of the head
    for rad, wid, lv in ARCS:
        box = [P(HX - rad, HY - rad), P(HX + rad, HY + rad)]
        d.arc(box, -ARC_HALF, ARC_HALF, fill=int(255 * lv), width=int(wid * SS))
    # the head: glow, then the core
    glow = Image.new("L", img.size, 0)
    gd = ImageDraw.Draw(glow)
    for r, v in ((12, 90), (8, 170), (5.5, 255)):
        gd.ellipse([P(HX - r, HY - r), P(HX + r, HY + r)], fill=v)
    glow = glow.filter(ImageFilter.GaussianBlur(SS * 2.0))
    img = Image.composite(Image.new("L", img.size, 255), img, glow)
    d = ImageDraw.Draw(img)
    r = 4.6
    d.ellipse([P(HX - r, HY - r), P(HX + r, HY + r)], fill=255)
    return img.resize((W, H), Image.LANCZOS)


def render_ink(nodes, links, head_links, dust):
    """The e-paper still: 1-bit ink, drawn like CometMark.cpp's paper path."""
    img = Image.new("1", (W, H), 1)
    d = ImageDraw.Draw(img)
    for a, b in links:
        na, nb = nodes[a], nodes[b]
        d.line([(round(na[0]), round(na[1])), (round(nb[0]), round(nb[1]))], fill=0, width=1)
    for n in head_links:
        nn = nodes[n]
        d.line([(HX, HY), (round(nn[0]), round(nn[1]))], fill=0, width=1)
    for x, y, r, _row, _dist in nodes:
        rr = max(1.5, r + 0.3)
        d.ellipse([round(x - rr), round(y - rr), round(x + rr) - 1, round(y + rr) - 1], fill=0)
    for x, y, _r, _dist in dust:
        d.rectangle([round(x) - 1, round(y) - 1, round(x), round(y)], fill=0)
    for rad, wid, _lv in ARCS:
        rr = rad + 1
        d.arc([HX - rr, HY - rr, HX + rr, HY + rr], -ARC_HALF, ARC_HALF, fill=0, width=2)
    d.ellipse([HX - 5, HY - 5, HX + 5, HY + 5], fill=0)
    d.ellipse([HX - 8, HY - 8, HX + 8, HY + 8], outline=0, width=1)
    return img


def write_bitmap(img, ink):
    px = list(img.get_flattened_data()) if hasattr(img, 'get_flattened_data') else list(img.getdata())
    lines = []
    for i in range(0, len(px), 12):
        row = []
        for v in px[i:i + 12]:
            r5, g6, b5 = v >> 3, v >> 2, v >> 3
            row.append("0x%04X" % ((r5 << 11) | (g6 << 5) | b5))
        lines.append("  " + ",".join(row) + ",")
    path = os.path.join(ROOT, "src", "comet_mark_rgb.h")
    with open(path, "w") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// The Comet mark, anti-aliased RGB565 (%dx%d, white-on-black). Generated by\n" % (W, H))
        f.write("// scripts/gen_comet_mark.py from the same geometry as ui-touch/CometMarkGeom.h.\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("#define COMET_MARK_W %d\n#define COMET_MARK_H %d\n" % (W, H))
        f.write("// One copy in the image: the pixels are defined only where COMET_MARK_DEFINE is set\n")
        f.write("// (ui-touch/UITask.cpp); everyone else sees the declaration.\n")
        f.write("extern const uint16_t COMET_MARK_RGB565[COMET_MARK_W * COMET_MARK_H];\n")
        f.write("#if defined(COMET_MARK_DEFINE)\n")
        f.write("const uint16_t COMET_MARK_RGB565[COMET_MARK_W * COMET_MARK_H] = {\n")
        f.write("\n".join(lines))
        f.write("\n};\n#endif\n")
        # the e-paper still, 1 bit per pixel (MSB first, 1 = ink), rows padded to bytes
        stride = (W + 7) // 8
        data = []
        for y in range(H):
            for bx in range(stride):
                v = 0
                for k in range(8):
                    x = bx * 8 + k
                    if x < W and ink.getpixel((x, y)) == 0:
                        v |= 0x80 >> k
                data.append(v)
        f.write("// The e-paper still (1 bit per pixel, MSB first, 1 = ink, %d bytes a row): the LVGL\n" % stride)
        f.write("// splash's paper frame, so the hand-off on e-paper does not change a pixel's meaning.\n")
        f.write("#define COMET_MARK_INK_STRIDE %d\n" % stride)
        f.write("extern const uint8_t COMET_MARK_INK1[COMET_MARK_INK_STRIDE * COMET_MARK_H];\n")
        f.write("#if defined(COMET_MARK_DEFINE)\n")
        f.write("const uint8_t COMET_MARK_INK1[COMET_MARK_INK_STRIDE * COMET_MARK_H] = {\n")
        for i in range(0, len(data), 16):
            f.write("  " + ",".join("0x%02X" % b for b in data[i:i + 16]) + ",\n")
        f.write("};\n#endif\n")


def write_geom(nodes, links, head_links, path, dust):
    p = os.path.join(ROOT, "src", "ui-touch", "CometMarkGeom.h")
    with open(p, "w") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// The Comet mark's geometry in its %d x %d box, in 1/16 units.\n" % (W, H))
        f.write("// Generated by scripts/gen_comet_mark.py (edit that, not this).\n#pragma once\n#include <stdint.h>\n\n")
        f.write("static const int16_t kCometBoxW = %d, kCometBoxH = %d;\n" % (W, H))
        f.write("static const int16_t kCometHeadX = %d, kCometHeadY = %d;   // 1/16 units\n" % (HX * 16, HY * 16))
        f.write("// A node: position and radius in 1/16 units, its light (0..255, by distance to the head).\n")
        f.write("struct CometNode { int16_t x, y, r; uint8_t light; };\n")
        f.write("static const CometNode kCometNodes[] = {\n")
        for x, y, r, _row, dist in nodes:
            f.write("  {%d, %d, %d, %d},\n" % (round(x * 16), round(y * 16), round(r * 16), round(255 * light(dist))))
        f.write("};\n")
        f.write("// Loose motes beside the band (no links): position, radius, light as for a node.\n")
        f.write("static const CometNode kCometDust[] = {\n")
        for x, y, r, dist in dust:
            f.write("  {%d, %d, %d, %d},\n" % (round(x * 16), round(y * 16), round(r * 16), round(255 * 0.8 * light(dist))))
        f.write("};\n")
        f.write("static const uint8_t kCometLinks[][2] = {\n")
        f.write("".join("  {%d, %d},\n" % ab for ab in links))
        f.write("};\n")
        f.write("static const uint8_t kCometHeadLinks[] = { %s };   // nodes linked to the head\n" % ", ".join(map(str, head_links)))
        f.write("static const uint8_t kCometPath[] = { %s };   // a message hopping in to the head\n" % ", ".join(map(str, path)))
        f.write("// The radio waves: radius and width in 1/16 units, light 0..255; half-angle in degrees.\n")
        f.write("struct CometArc { int16_t r, w; uint8_t light; };\n")
        f.write("static const CometArc kCometArcs[] = {\n")
        for rad, wid, lv in ARCS:
            f.write("  {%d, %d, %d},\n" % (rad * 16, round(wid * 16), round(255 * lv)))
        f.write("};\n")
        f.write("static const int16_t kCometArcHalfDeg = %d;\n" % round(ARC_HALF))


def write_svg(nodes, links, head_links, dust):
    green = "#2bff88"
    out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">' % (W, H, W * 2, H * 2),
           '<defs><radialGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset=".35" stop-color="%s" stop-opacity=".85"/>'
           '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient></defs>' % (green, green)]
    for a, b in links:
        na, nb = nodes[a], nodes[b]
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.1" stroke-opacity="%.2f"/>'
                   % (na[0], na[1], nb[0], nb[1], green, 0.6 * light((na[4] + nb[4]) / 2)))
    for n in head_links:
        nn = nodes[n]
        out.append('<line x1="%d" y1="%d" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.3" stroke-opacity=".8"/>'
                   % (HX, HY, nn[0], nn[1], green))
    for x, y, r, _row, dist in nodes:
        out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" fill-opacity="%.2f"/>'
                   % (x, y, r, green, min(1.0, 0.35 + light(dist))))
    for x, y, r, dist in dust:
        out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" fill-opacity="%.2f"/>' % (x, y, r, green, 0.8 * light(dist)))
    for rad, wid, lv in ARCS:
        a0, a1 = math.radians(-ARC_HALF), math.radians(ARC_HALF)
        x0, y0 = HX + rad * math.cos(a0), HY + rad * math.sin(a0)
        x1, y1 = HX + rad * math.cos(a1), HY + rad * math.sin(a1)
        out.append('<path d="M%.1f %.1f A%d %d 0 0 1 %.1f %.1f" fill="none" stroke="%s" stroke-width="%.1f" '
                   'stroke-linecap="round" stroke-opacity="%.2f"/>' % (x0, y0, rad, rad, x1, y1, green, wid, lv))
    out.append('<circle cx="%d" cy="%d" r="13" fill="url(#g)"/>' % (HX, HY))
    out.append('<circle cx="%d" cy="%d" r="4.6" fill="#fff"/>' % (HX, HY))
    out.append("</svg>")
    with open(os.path.join(ROOT, "assets", "comet-mark.svg"), "w") as f:
        f.write("\n".join(out) + "\n")


def write_favicon(img):
    # The head and the waves, square: the full mark is too wide for 16 px.
    sq = img.crop((HX - 40, HY - 40, HX + 40, HY + 40)).resize((64, 64), Image.LANCZOS)
    rgb = Image.new("RGB", sq.size, (6, 10, 8))
    tint = Image.new("RGB", sq.size, (43, 255, 136))
    rgb = Image.composite(tint, rgb, sq)
    rgb.save(os.path.join(ROOT, "assets", "favicon.ico"), sizes=[(16, 16), (32, 32), (48, 48)])


def write_placeholder(img):
    from PIL import ImageFont
    PW, PH = 320, 240
    bg = Image.new("RGB", (PW, PH))
    top, bot = (6, 26, 16), (1, 4, 3)
    for y in range(PH):
        f = y / (PH - 1)
        c = tuple(round(top[i] + (bot[i] - top[i]) * f) for i in range(3))
        ImageDraw.Draw(bg).line([(0, y), (PW, y)], fill=c)
    # a faint wide glow behind the head
    glow = Image.new("L", (PW, PH), 0)
    ox, oy = (PW - W) // 2, 58
    ImageDraw.Draw(glow).ellipse([ox + HX - 60, oy + HY - 45, ox + HX + 60, oy + HY + 45], fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(28))
    bg = Image.composite(Image.new("RGB", (PW, PH), (43, 255, 136)), bg, glow)
    tint = Image.new("RGB", img.size, (170, 255, 205))
    mask = Image.new("L", (PW, PH), 0)
    mask.paste(img, (ox, oy))
    bg = Image.composite(Image.new("RGB", (PW, PH), (210, 255, 228)), bg, mask)
    d = ImageDraw.Draw(bg)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    word, gap = "COMET", 9
    widths = [d.textlength(ch, font=font) for ch in word]
    x = (PW - (sum(widths) + gap * (len(word) - 1))) / 2
    for ch, w in zip(word, widths):
        d.text((x, oy + H + 14), ch, font=font, fill=(236, 244, 240))
        x += w + gap
    import io
    buf = io.BytesIO()
    bg.save(buf, "JPEG", quality=88, optimize=False, progressive=False)
    data = buf.getvalue()
    path = os.path.join(ROOT, "src", "ui-touch", "assets", "lockscreen_placeholder_jpg.h")
    with open(path, "w") as f:
        f.write("// Generated by scripts/gen_comet_mark.py -- do not edit by hand.\n")
        f.write("// 320x240 JPEG lock wallpaper (the Comet mark and wordmark on a dark green ground),\n")
        f.write("// seeded to SPIFFS /lock/placeholder.jpg for the picture picker, and the fallback\n")
        f.write("// when a chosen wallpaper cannot be read.\n")
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write("static const uint8_t lockscreen_placeholder_jpg[] = {\n")
        for i in range(0, len(data), 16):
            f.write("  " + ",".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
        f.write("};\nstatic const size_t lockscreen_placeholder_jpg_len = %d;\n" % len(data))
    bg.save(os.path.join(os.environ.get("TMPDIR", "/tmp"), "lock_placeholder_preview.png"))


if __name__ == "__main__":
    nodes, links, head_links, path, dust = geometry()
    assert len(nodes) < 256
    write_geom(nodes, links, head_links, path, dust)
    img = render_bitmap(nodes, links, head_links, dust)
    ink = render_ink(nodes, links, head_links, dust)
    write_bitmap(img, ink)
    ink.convert("L").resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(os.environ.get("TMPDIR", "/tmp"), "comet_mark_ink.png"))
    write_svg(nodes, links, head_links, dust)
    write_favicon(img)
    write_placeholder(img)
    img.resize((W * 4, H * 4), Image.NEAREST).save(os.path.join(os.environ.get("TMPDIR", "/tmp"), "comet_mark_preview.png"))
    print("wrote CometMarkGeom.h, comet_mark_rgb.h, assets/comet-mark.svg, assets/favicon.ico (%d nodes, %d links)"
          % (len(nodes), len(links)))
