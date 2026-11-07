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

Run:  python3 scripts/gen_comet_mark.py   (needs Pillow and SciPy)
"""
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

W, H = 208, 110          # the box (drawing units = pixels at scale 1)
HX, HY = 172, 74         # the head
AXIS = 194.0             # tail direction, degrees (0 = right, y grows down: 194 = left, a little up)
LENGTH = 176.0           # tail length
BEND = 16.0             # the tail sweeps in a gentle curve, px at its end
WIDTH = 30.0             # the tail's half-width at its end
SPACING = 13.0           # rough distance between nodes (tighter near the head)
DUST = 12                # loose motes beside the tail: no links
# The bow wave in front of the head (comets have one; here it is also the radio):
# radius, width, light.
ARCS = [(11, 2.0, 0.85), (18, 1.6, 0.42)]
ARC_HALF = 62.0          # half the opening angle of a wave, degrees
SPINE = 18               # samples of the tail's centre line, for its glow
SS = 6                   # supersampling for the bitmap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def along(s, off):
    """A point s (0 head .. 1 end) along the tail's curved centre line, off px across it."""
    a = math.radians(AXIS)
    dx, dy = math.cos(a), math.sin(a)
    nx, ny = -dy, dx                       # the normal (points "up" for this axis)
    bend = BEND * s * s
    return (HX + dx * LENGTH * s + nx * (bend + off), HY + dy * LENGTH * s + ny * (bend + off))


def half_width(s):
    """The tail opens from the head and keeps widening as it streams away."""
    s = max(0.0, min(1.0, s))
    return 1.5 + WIDTH * s ** 0.8


# The tail's rays: where each ends across the tail (in half-widths), how far it reaches
# (along), and where its first node sits.
RAYS = [(-1.0, 0.80, 0.24), (-0.48, 0.95, 0.15), (0.02, 1.00, 0.08), (0.52, 0.90, 0.19), (1.0, 0.74, 0.30)]


def geometry():
    import heapq
    rnd = random.Random(5)
    nodes = []           # (x, y, r, s, dist, ray)
    rays = []
    for ri, (end_off, reach, first) in enumerate(RAYS):
        ray = []
        sv = first
        while sv <= reach + 1e-6:
            off = end_off * half_width(sv) * 0.95 + rnd.uniform(-0.8, 0.8)
            x, y = along(sv + rnd.uniform(-0.01, 0.01), off)
            dist = math.hypot(x - HX, y - HY)
            r = 0.8 + 1.5 * max(0.0, 1.0 - dist / 160.0) ** 1.1
            ray.append(len(nodes))
            nodes.append((x, y, r, sv, dist, ri))
            sv += 0.075 + 0.12 * sv     # the hops lengthen down the tail
        rays.append(ray)
    P = [(n[0], n[1]) for n in nodes]
    links = set()
    for ray in rays:                 # each ray: a stream of hops
        for a_, b_ in zip(ray, ray[1:]):
            links.add(tuple(sorted((a_, b_))))
    # cross links between neighbouring rays: the mesh between the streams
    for ri in range(len(rays) - 1):
        for n in rays[ri][::2]:
            if nodes[n][3] > 0.62:
                continue   # the far tail streams free: rays, not a ladder
            ahead = [j for j in rays[ri + 1] if nodes[j][3] > nodes[n][3]] or rays[ri + 1]
            m = min(ahead, key=lambda j: math.dist(P[n], P[j]))   # diagonal: woven, not a ladder
            if math.dist(P[n], P[m]) < SPACING * (0.8 + 0.9 * nodes[n][3]):
                links.add(tuple(sorted((n, m))))
    links = sorted(links)
    head_links = [ray[0] for ray in rays[1:4]]   # the inner rays start at the head
    # The message's way in: the shortest hop path from the end of the longest ray.
    adj = {i: [] for i in range(len(nodes))}
    for a_, b_ in links:
        w = math.dist(P[a_], P[b_])
        adj[a_].append((b_, w)); adj[b_].append((a_, w))
    start = rays[2][-1]
    best_d = {start: 0.0}; prev = {}
    q = [(0.0, start)]
    while q:
        d, u = heapq.heappop(q)
        if d > best_d.get(u, 1e9):
            continue
        for v, w in adj[u]:
            c = d + w + 4.0
            if c < best_d.get(v, 1e9):
                best_d[v] = c; prev[v] = u
                heapq.heappush(q, (c, v))
    end = min(head_links, key=lambda i: best_d.get(i, 1e9))
    path = [end]
    while path[-1] != start:
        path.append(prev[path[-1]])
    path.reverse()
    dust = []
    while len(dust) < DUST:
        s_ = rnd.uniform(0.2, 1.0)
        off = rnd.uniform(-1.25, 1.25) * half_width(s_)
        x, y = along(s_, off)
        if 3 < x < W - 3 and 3 < y < H - 3 and all(math.dist((x, y), p_) > 4 for p_ in P):
            dust.append((x, y, rnd.uniform(0.55, 0.9), math.hypot(x - HX, y - HY)))
    spine = []
    for k in range(SPINE + 1):
        s_ = k / SPINE
        x, y = along(s_, 0)
        spine.append((x, y, half_width(s_), max(0.0, 1.0 - s_) ** 1.4))
    nodes = [n[:5] for n in nodes]
    return nodes, links, head_links, path, dust, spine


def light(d):
    """How bright a part of the tail is, from its distance to the head (1 near, ~0.25 far)."""
    return 0.18 + 0.82 * max(0.0, 1.0 - d / 165.0) ** 1.5


def tail_glow(spine, scale, peak):
    """The tail's own soft light behind the mesh: a blurred, fading band."""
    g = Image.new("L", (W * scale, H * scale), 0)
    gd = ImageDraw.Draw(g)
    for (x0, y0, w0, l0), (x1, y1, w1, l1) in zip(spine, spine[1:]):
        v = int(peak * (l0 + l1) / 2)
        gd.line([(x0 * scale, y0 * scale), (x1 * scale, y1 * scale)], fill=v, width=int((w0 + w1) * 0.85 * scale))
        rr = w1 * 0.85 * scale
        gd.ellipse([x1 * scale - rr, y1 * scale - rr, x1 * scale + rr, y1 * scale + rr], fill=v)
    return g.filter(ImageFilter.GaussianBlur(scale * 9))


def render_bitmap(nodes, links, head_links, dust, spine):
    img = tail_glow(spine, SS, 120)
    d = ImageDraw.Draw(img)

    def P(x, y):
        return (x * SS, y * SS)

    for a, b in links:
        na, nb = nodes[a], nodes[b]
        v = int(255 * 0.55 * light((na[4] + nb[4]) / 2))
        d.line([P(na[0], na[1]), P(nb[0], nb[1])], fill=v, width=int(1.1 * SS), joint="curve")
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
    for r, v in ((16, 70), (10, 160), (6, 255)):
        gd.ellipse([P(HX - r, HY - r), P(HX + r, HY + r)], fill=v)
    glow = glow.filter(ImageFilter.GaussianBlur(SS * 2.0))
    img = Image.composite(Image.new("L", img.size, 255), img, glow)
    d = ImageDraw.Draw(img)
    r = 4.6
    d.ellipse([P(HX - r, HY - r), P(HX + r, HY + r)], fill=255)
    return img.resize((W, H), Image.LANCZOS)


def render_ink(nodes, links, head_links, dust):
    """The e-paper still: 1-bit ink. Drawn large and thresholded, so every node is a
    round dot and every line an even stroke (no stray pixels from a 1-px renderer)."""
    S = 8
    img = Image.new("L", (W * S, H * S), 255)
    d = ImageDraw.Draw(img)

    def P(x, y):
        return (x * S, y * S)

    def dot(x, y, r):
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=0)

    for a, b in links:
        na, nb = nodes[a], nodes[b]
        d.line([P(na[0], na[1]), P(nb[0], nb[1])], fill=0, width=int(1.05 * S))
    for n in head_links:
        nn = nodes[n]
        d.line([P(HX, HY), P(nn[0], nn[1])], fill=0, width=int(1.3 * S))
    def paper_dot(x, y, r):   # a dot with a hairline of paper around it, so lines never blob into it
        d.ellipse([P(x - r - 0.9, y - r - 0.9), P(x + r + 0.9, y + r + 0.9)], fill=255)
        dot(x, y, r)

    for x, y, r, _s, _dist in nodes:
        paper_dot(x, y, max(1.5, r * 1.15 + 0.35))
    for x, y, r_, _dist in dust:   # only the clear motes: a speck too near a line reads as a stray pixel
        if r_ >= 0.7 and all(math.dist((x, y), (nn[0], nn[1])) > 6 for nn in nodes):
            dot(x, y, 1.0)
    for rad, wid, _lv in ARCS:
        rr = rad + 1
        d.arc([P(HX - rr, HY - rr), P(HX + rr, HY + rr)], -ARC_HALF, ARC_HALF, fill=0, width=int(2.0 * S))
        for ang in (-ARC_HALF, ARC_HALF):   # round ends
            ex = HX + (rr - 1.0) * math.cos(math.radians(ang)); ey = HY + (rr - 1.0) * math.sin(math.radians(ang))
            dot(ex, ey, 1.0)
    d.ellipse([P(HX - 7.0, HY - 7.0), P(HX + 7.0, HY + 7.0)], fill=255)
    dot(HX, HY, 5.4)
    small = img.resize((W, H), Image.BOX)
    return small.point(lambda v: 0 if v < 140 else 255).convert("1")


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


def write_glow(spine):
    """The tail's soft light as an 8-bit alpha image at the mark's own size, which the
    drawing tints: LVGL cannot blur, but it can lay down a blurred picture."""
    g = tail_glow(spine, 2, 255).resize((W, H), Image.LANCZOS)
    mx = max(g.get_flattened_data() if hasattr(g, 'get_flattened_data') else g.getdata()) or 1
    g = g.point(lambda v: min(255, round(v * 255 / mx)))
    px = list(g.get_flattened_data()) if hasattr(g, 'get_flattened_data') else list(g.getdata())
    with open(os.path.join(ROOT, "src", "ui-touch", "CometMarkGlow.h"), "w") as f:
        f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        f.write("// The Comet mark's tail glow: 8-bit alpha, %d x %d (the mark's box).\n" % (W, H))
        f.write("// Generated by scripts/gen_comet_mark.py (edit that, not this).\n#pragma once\n#include <stdint.h>\n\n")
        f.write("static const int16_t kCometGlowW = %d, kCometGlowH = %d;\n" % (W, H))
        f.write("static const uint8_t kCometGlowA8[] = {\n")
        for i in range(0, len(px), 20):
            f.write("  " + ",".join(str(v) for v in px[i:i + 20]) + ",\n")
        f.write("};\n")


def write_geom(nodes, links, head_links, path, dust, spine):
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
        f.write("// The tail's centre line, head to end: position and half-width in 1/16 units, light 0..255.\n")
        f.write("struct CometSpine { int16_t x, y, hw; uint8_t light; };\n")
        f.write("static const CometSpine kCometSpine[] = {\n")
        for x, y, hw, lv in spine:
            f.write("  {%d, %d, %d, %d},\n" % (round(x * 16), round(y * 16), round(hw * 16), round(255 * lv)))
        f.write("};\n")


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
    nodes, links, head_links, path, dust, spine = geometry()
    assert len(nodes) < 256
    write_geom(nodes, links, head_links, path, dust, spine)
    write_glow(spine)
    img = render_bitmap(nodes, links, head_links, dust, spine)
    ink = render_ink(nodes, links, head_links, dust)
    write_bitmap(img, ink)
    ink.convert("L").resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(os.environ.get("TMPDIR", "/tmp"), "comet_mark_ink.png"))
    write_svg(nodes, links, head_links, dust)
    write_favicon(img)
    write_placeholder(img)
    img.resize((W * 4, H * 4), Image.NEAREST).save(os.path.join(os.environ.get("TMPDIR", "/tmp"), "comet_mark_preview.png"))
    print("wrote CometMarkGeom.h, comet_mark_rgb.h, assets/comet-mark.svg, assets/favicon.ico (%d nodes, %d links)"
          % (len(nodes), len(links)))
