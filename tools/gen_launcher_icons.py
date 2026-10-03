#!/usr/bin/env python3
"""Generate the launcher's four icon glyphs as 8-bit-alpha LVGL v9 images.

Geometry is taken verbatim from Lucide (https://lucide.dev, lucide-static
v1.50.0, ISC licensed): 24x24 grid, 2px stroke, round caps and joins. That
stroke-only look stays legible at 40px on a 320x240 panel, where the previous
filled/lv_obj glyphs went muddy.

Because the paths lean on elliptical arcs (book-open and the gear are almost
entirely 'a' commands), this script carries a small SVG path renderer rather
than approximating the shapes by hand: it parses M/L/H/V/C/S/Q/T/A/Z (absolute
and relative), converts each arc to Beziers via the endpoint->centre
parameterisation from the SVG spec, flattens the curves, then strokes with
round joins. Drawing happens at 4x and is downscaled with LANCZOS, so curves
keep a grayscale antialias ramp that a 1bpp bitmap could not hold.

Output is a single alpha plane, so the UI tints one bitmap to any colour with
lv_obj_set_style_img_recolor_* — that is how the focused tile gets an accent
icon without a second copy of the art.

Run:  python tools/gen_launcher_icons.py
Out:  components/app/ui/ui_launcher_icons.{h,c}
"""
import math
import os
import re
import sys

from PIL import Image, ImageDraw

SIZE = 40          # final glyph edge, matches UI_TILE_ICON
SS = 4             # supersampling factor
VIEW = 24.0        # Lucide's design grid
FIT = 0.82         # fraction of the tile's icon box the art occupies

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "components", "app", "ui", "ui_launcher_icons.c")
OUT_H = os.path.join(ROOT, "components", "app", "ui", "ui_launcher_icons.h")

# Lucide path data, copied verbatim from lucide-static v1.50.0.
LUCIDE = {
    "music": {"stroke": 2, "els": [
        ("path", "M9 18V5l12-2v13"),
        ("circle", 6, 18, 3),
        ("circle", 18, 16, 3),
    ]},
    "ebook": {"stroke": 2, "els": [
        ("path", "M12 5v16"),
        ("path", "M20.001 19A2 2 0 0022 17V5a2 2 0 00-1.999-2L16 3.002"
                 "A5 5 0 0012 5a5 5 0 00-4-2H4a2 2 0 00-2 2v12a2 2 0 00"
                 "1.999 2H8a5 5 0 014 2 5 5 0 014-2z"),
    ]},
    "gear": {"stroke": 2, "els": [
        ("path", "M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 "
                 "3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 "
                 "0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 "
                 "1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 "
                 "2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 "
                 "2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915"),
        ("circle", 12, 12, 3),
    ]},
    "bt": {"stroke": 2, "els": [
        ("path", "m7 7 10 10-5 5V2l5 5L7 17"),
    ]},
}

GLYPH_CN = {"music": "音乐", "ebook": "电子书", "gear": "设置", "bt": "蓝牙"}

# ---------------------------------------------------------------- parsing --

_ARGS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4,
         "Q": 4, "T": 2, "A": 7, "Z": 0}

_NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def _skip(s, i):
    """Skip separators: whitespace and at most one comma between numbers."""
    while i < len(s) and (s[i].isspace() or s[i] == ","):
        i += 1
    return i


def _num(s, i):
    i = _skip(s, i)
    m = _NUM.match(s, i)
    if not m:
        raise ValueError("expected a number at %d of %r" % (i, s))
    return float(m.group(0)), m.end()


def _flag(s, i):
    """Read one arc flag. Per the SVG grammar these are single characters and
    may sit flush against the next number ("A2 2 0 0022 17" is rx=2 ry=2 rot=0
    laf=0 sf=0 x=22 y=17), so they must not be read by the number regex."""
    i = _skip(s, i)
    if i < len(s) and s[i] in "01":
        return (1 if s[i] == "1" else 0), i + 1
    v, i = _num(s, i)
    return int(v), i


def _split_cmds(d):
    """Split into (command, argument-text) pairs; argument text keeps the
    original spacing so flags and numbers stay distinguishable."""
    out = []
    for ch in d:
        if ch.isalpha():
            out.append((ch, []))
        elif out:
            out[-1][1].append(ch)
        else:
            raise ValueError("path data starts with a number: %r" % d)
    return [(c, "".join(a)) for c, a in out]


def _flatten_cubic(p0, p1, p2, p3, n=24):
    out = []
    for k in range(n + 1):
        t = k / n
        m = 1.0 - t
        a, b, c, e = m * m * m, 3 * m * m * t, 3 * m * t * t, t * t * t
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + e * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + e * p3[1]))
    return out


def _flatten_quad(p0, p1, p2, n=20):
    out = []
    for k in range(n + 1):
        t = k / n
        m = 1.0 - t
        a, b, c = m * m, 2 * m * t, t * t
        out.append((a * p0[0] + b * p1[0] + c * p2[0],
                    a * p0[1] + b * p1[1] + c * p2[1]))
    return out


def _arc_to_beziers(x0, y0, rx, ry, phi_deg, large, sweep, x1, y1):
    """SVG spec F.6.5: endpoint -> centre parameterisation, then split into
    <=90 degree Bezier segments."""
    if rx == 0 or ry == 0 or (x0 == x1 and y0 == y1):
        return [((x0, y0), (x1, y1), (x1, y1), (x1, y1))]
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    xp, yp = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = (xp * xp) / (rx * rx) + (yp * yp) / (ry * ry)
    if lam > 1:
        k = math.sqrt(lam)
        rx, ry = rx * k, ry * k
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp = co * rx * yp / ry
    cyp = -co * ry * xp / rx
    cx = cp * cxp - sp * cyp + (x0 + x1) / 2.0
    cy = sp * cxp + cp * cyp + (y0 + y1) / 2.0

    def angle(ux, uy, vx, vy):
        n = math.hypot(ux, uy) * math.hypot(vx, vy)
        if n == 0:
            return 0.0
        c = max(-1.0, min(1.0, (ux * vx + uy * vy) / n))
        a = math.acos(c)
        return -a if ux * vy - uy * vx < 0 else a

    th1 = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    dth = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and dth > 0:
        dth -= 2 * math.pi
    elif sweep and dth < 0:
        dth += 2 * math.pi

    segs = max(1, int(math.ceil(abs(dth) / (math.pi / 2))))
    delta = dth / segs
    k = 4.0 / 3.0 * math.tan(delta / 4.0)
    out = []
    th = th1
    for _ in range(segs):
        th2 = th + delta
        c1, s1 = math.cos(th), math.sin(th)
        c2, s2 = math.cos(th2), math.sin(th2)
        p1 = (cx + rx * c1 * cp - ry * s1 * sp, cy + rx * c1 * sp + ry * s1 * cp)
        p2 = (cx + rx * c2 * cp - ry * s2 * sp, cy + rx * c2 * sp + ry * s2 * cp)
        d1 = (-rx * c1 * sp - ry * s1 * cp, -rx * c1 * cp + ry * s1 * sp)
        d2 = (-rx * c2 * sp - ry * s2 * cp, -rx * c2 * cp + ry * s2 * sp)
        out.append((p1,
                    (p1[0] + k * d1[0], p1[1] + k * d1[1]),
                    (p2[0] - k * d2[0], p2[1] - k * d2[1]),
                    p2))
        th = th2
    return out


def parse_path(d):
    """Return [(points, closed), ...] in user units, curves already flattened."""
    subs = []
    pts = []
    cx = cy = sx = sy = 0.0
    prev_c2 = None      # reflection state for S/s
    prev_q = None       # reflection state for T/t

    for cmd0, s in _split_cmds(d):
        if cmd0.upper() not in ("C", "S"):
            prev_c2 = None
        if cmd0.upper() not in ("Q", "T"):
            prev_q = None

        c = cmd0
        i = 0
        while _skip(s, i) < len(s):
            if c in "Zz":
                break
            if c in "Aa":
                rx, i = _num(s, i)
                ry, i = _num(s, i)
                rot, i = _num(s, i)
                laf, i = _flag(s, i)
                sf, i = _flag(s, i)
                x, i = _num(s, i)
                y, i = _num(s, i)
                a = [rx, ry, rot, laf, sf, x, y]
            else:
                a = []
                for _ in range(_ARGS[c.upper()]):
                    v, i = _num(s, i)
                    a.append(v)

            if c in "Mm":
                x, y = a
                if c == "m":
                    x, y = x + cx, y + cy
                if pts:
                    subs.append((pts, False))
                pts = [(x, y)]
                cx = sx = x
                cy = sy = y
                c = "L" if c == "M" else "l"      # implicit repeats are lineto
            elif c in "Ll":
                x, y = a
                if c == "l":
                    x, y = x + cx, y + cy
                pts.append((x, y))
                cx, cy = x, y
            elif c in "Hh":
                x = a[0] + (cx if c == "h" else 0.0)
                pts.append((x, cy))
                cx = x
            elif c in "Vv":
                y = a[0] + (cy if c == "v" else 0.0)
                pts.append((cx, y))
                cy = y
            elif c in "Cc":
                x1, y1, x2, y2, x, y = a
                if c == "c":
                    x1, y1 = x1 + cx, y1 + cy
                    x2, y2 = x2 + cx, y2 + cy
                    x, y = x + cx, y + cy
                pts.extend(_flatten_cubic((cx, cy), (x1, y1), (x2, y2), (x, y))[:-1])
                prev_c2 = (x2, y2)
                cx, cy = x, y
            elif c in "Ss":
                x2, y2, x, y = a
                if c == "s":
                    x2, y2, x, y = x2 + cx, y2 + cy, x + cx, y + cy
                x1, y1 = (2 * cx - prev_c2[0], 2 * cy - prev_c2[1]) if prev_c2 else (cx, cy)
                pts.extend(_flatten_cubic((cx, cy), (x1, y1), (x2, y2), (x, y))[:-1])
                prev_c2 = (x2, y2)
                cx, cy = x, y
            elif c in "Qq":
                x1, y1, x, y = a
                if c == "q":
                    x1, y1, x, y = x1 + cx, y1 + cy, x + cx, y + cy
                pts.extend(_flatten_quad((cx, cy), (x1, y1), (x, y))[:-1])
                prev_q = (x1, y1)
                cx, cy = x, y
            elif c in "Tt":
                x, y = a
                if c == "t":
                    x, y = x + cx, y + cy
                x1, y1 = (2 * cx - prev_q[0], 2 * cy - prev_q[1]) if prev_q else (cx, cy)
                pts.extend(_flatten_quad((cx, cy), (x1, y1), (x, y))[:-1])
                prev_q = (x1, y1)
                cx, cy = x, y
            elif c in "Aa":
                rx, ry, rot, laf, sf, x, y = a
                if c == "a":
                    x, y = x + cx, y + cy
                for seg in _arc_to_beziers(cx, cy, rx, ry, rot, laf, sf, x, y):
                    pts.extend(_flatten_cubic(*seg)[:-1])
                cx, cy = x, y

        if cmd0 in "Zz":
            if pts:
                subs.append((pts, True))
                pts = []
            cx, cy = sx, sy

    if pts:
        subs.append((pts, False))
    return subs


# ------------------------------------------------------------ rasterising --

def _stroke(d, pts, w, closed):
    """Stroke a flattened polyline with round joins and round caps: a disc at
    every vertex reproduces linejoin=round and supplies the caps."""
    r = w / 2.0
    if len(pts) == 1:
        x, y = pts[0]
        d.ellipse([x - r, y - r, x + r, y + r], fill=255)
        return
    segs = list(zip(pts, pts[1:]))
    if closed:
        segs.append((pts[-1], pts[0]))
    lw = max(1, int(round(w)))
    for a, b in segs:
        d.line([a, b], fill=255, width=lw)
    for x, y in pts:
        d.ellipse([x - r, y - r, x + r, y + r], fill=255)


def _center_ink(img, thresh=8):
    """Shift the artwork so its ink bounding box sits exactly on the canvas
    centre. A glyph's geometric centre and its visual centre differ by a
    pixel here and there, and on a 40px tile that reads as "off-centre"."""
    px = img.load()
    xs, ys = [], []
    for y in range(SIZE):
        for x in range(SIZE):
            if px[x, y] > thresh:
                xs.append(x)
                ys.append(y)
    if not xs:
        return img
    dx = int(round((SIZE - 1 - (min(xs) + max(xs))) / 2.0))
    dy = int(round((SIZE - 1 - (min(ys) + max(ys))) / 2.0))
    if dx == 0 and dy == 0:
        return img
    out = Image.new("L", (SIZE, SIZE), 0)
    out.paste(img, (dx, dy))
    return out


def render(name):
    spec = LUCIDE[name]
    w = SIZE * SS
    img = Image.new("L", (w, w), 0)
    d = ImageDraw.Draw(img)
    scale = (w * FIT) / VIEW
    off = (w - w * FIT) / 2.0
    lw = spec["stroke"] * scale

    def tf(p):
        return (p[0] * scale + off, p[1] * scale + off)

    for el in spec["els"]:
        if el[0] == "path":
            for pts, closed in parse_path(el[1]):
                _stroke(d, [tf(p) for p in pts], lw, closed)
        else:
            _, ccx, ccy, r = el
            ring = [(ccx + r * math.cos(2 * math.pi * k / 64),
                     ccy + r * math.sin(2 * math.pi * k / 64)) for k in range(64)]
            _stroke(d, [tf(p) for p in ring], lw, True)
    return _center_ink(img.resize((SIZE, SIZE), Image.LANCZOS))


# ----------------------------------------------------------------- output --

def emit_array(out, name, img):
    out.append("const uint8_t %s_data[%d] = {" % (name, SIZE * SIZE))
    px = list(img.getdata())
    for row in range(SIZE):
        out.append("    " + " ".join("0x%02x," % v for v in px[row * SIZE:(row + 1) * SIZE]))
    out.append("};")
    out.append("")


def main():
    order = ["music", "ebook", "gear", "bt"]
    images = [(n, render(n)) for n in order]

    hdr = [
        "/* Generated by tools/gen_launcher_icons.py — do not edit by hand.",
        " *",
        " * Launcher glyphs taken from Lucide (lucide-static v1.50.0, ISC):",
        " * 24x24 grid, 2px stroke, round caps/joins. Emitted as 8-bit-alpha LVGL",
        " * v9 images (LV_COLOR_FORMAT_A8, one byte per pixel) rendered at 4x and",
        " * LANCZOS-downscaled, so curves keep a grayscale antialias ramp.",
        " *",
        " * They carry no colour: the launcher tints them through",
        " * lv_obj_set_style_img_recolor_*, which is how the focused tile's icon",
        " * turns accent-coloured without a second copy of the art.",
        " */",
        "#ifndef UI_LAUNCHER_ICONS_H",
        "#define UI_LAUNCHER_ICONS_H",
        "",
        "#include \"lvgl.h\"",
        "",
    ]
    hdr += ["extern const lv_image_dsc_t ui_icon_%s;" % n for n in order]
    hdr += ["", "#endif /* UI_LAUNCHER_ICONS_H */", ""]

    c = ["/* Generated by tools/gen_launcher_icons.py — do not edit by hand. */",
         "#include \"ui_launcher_icons.h\"", ""]
    for name, img in images:
        emit_array(c, "ui_icon_%s" % name, img)
    for name, _img in images:
        c.append(
            "const lv_image_dsc_t ui_icon_%s = {\n"
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,\n"
            "    .header.cf    = LV_COLOR_FORMAT_A8,\n"
            "    .header.flags = 0,\n"
            "    .header.w     = %d,\n"
            "    .header.h     = %d,\n"
            "    .header.stride = %d,\n"
            "    .data_size    = sizeof(ui_icon_%s_data),\n"
            "    .data         = ui_icon_%s_data,\n"
            "};\n" % (name, SIZE, SIZE, SIZE, name, name))

    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(hdr))
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(c))

    print("wrote %s (%d bytes)" % (OUT_H, os.path.getsize(OUT_H)))
    print("wrote %s (%d bytes)" % (OUT_C, os.path.getsize(OUT_C)))
    for name, img in images:
        px = list(img.getdata())
        ink = sum(1 for v in px if v > 128)
        print("  %-6s %-8s ink %4d px (%4.1f%%), max alpha %3d"
              % (name, GLYPH_CN[name], ink, 100.0 * ink / len(px), max(px)))


if __name__ == "__main__":
    sys.exit(main())
