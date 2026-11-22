#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Rumble Jungle - perfboard layout drawing.

Writes docs/perfboard.svg: the component side of a 70 x 90 mm perfboard with
the Waveshare ESP32-P4-ETH and two CJMCU-1808 (PCM1808) ADC modules, plus
the solder side (mirrored) with every wire. Pure Python, no dependencies:

    python3 hardware/perfboard.py

The geometry shared with the enclosure lives in layout.py.
"""
import os
from xml.sax.saxutils import escape

import layout as L

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "docs", "perfboard.svg")

S = 8.0                              # px per mm
P = L.PITCH

# --------------------------------------------------------------------------
# Nets and colours
# --------------------------------------------------------------------------
NETS = {
    "GND":  ("#1f2933", "Ground"),
    "5V":   ("#d64545", "5 V (VSYS) before the filter"),
    "5VF":  ("#f0803c", "5 V filtered, ADC analog supply"),
    "3V3":  ("#e8b90c", "3.3 V, ADC digital supply"),
    "MCLK": ("#2f80ed", "MCLK  GPIO20 -> SCK"),
    "BCK":  ("#9b51e0", "BCK  GPIO21 -> BCK"),
    "LRCK": ("#27ae60", "LRCK  GPIO22 -> LRC"),
    "DA":   ("#00a3a3", "DATA A  OUT (ADC A) -> GPIO23"),
    "DB":   ("#e0569b", "DATA B  OUT (ADC B) -> GPIO26"),
}

# --------------------------------------------------------------------------
# The modules
# --------------------------------------------------------------------------

# ESP32-P4-ETH header, seen from the top with the RJ45 to the right:
# row ESP_ROW_TOP carries pins 40..21 from left to right, ESP_ROW_BOT 1..20.
ESP_PINS_TOP = ["VBUS", "VSYS", "GND", "3V3_EN", "3V3", "20", "21", "GND", "22",
                "23", "RUN", "26", "GND", "27", "32", "33", "46", "GND", "47", "48"]
ESP_PINS_BOT = ["54", "19", "GND", "18", "17", "16", "15", "GND", "14", "6",
                "5", "4", "GND", "3", "2", "8", "7", "GND", "24", "25"]


def esp_pin(name, row="top"):
    pins = ESP_PINS_TOP if row == "top" else ESP_PINS_BOT
    r = L.ESP_ROW_TOP if row == "top" else L.ESP_ROW_BOT
    return (r, L.ESP_COL + pins.index(name))


# CJMCU-1808 module, 35 x 27 mm. Pin positions relative to the module's
# top-left corner as seen from above, back header on top, in pitch units
# from the first back-row pin (which lands on a perfboard hole).
ADC_W, ADC_H = 35.0, 27.0
ADC_FIRST = (2.1, 2.9)                       # first back-row pin from the corner, mm
ADC_BACK = ["OUT", "BCK", "LRC", "SCK", "GND", "3.3V"]          # (row 0, cols 0..5)
ADC_FRONT = ["FMY", "MD1", "MD0", "GND", "3.3", "+5V"]          # (row 9, cols 4..9)
ADC_FRONT_COL0, ADC_ROWS = 4, 9
ADC_ANALOG = ["LIN", "-", "RIN"]                                 # on the left edge, wired by hand


class Adc:
    """An ADC module placed with its first back-row pin at (row, col),
    optionally turned by 180 degrees."""

    def __init__(self, ref, name, row, col, turned=False):
        self.ref, self.name, self.row, self.col, self.turned = ref, name, row, col, turned

    def pin(self, label, front=None):
        if front is None:
            front = label in ADC_FRONT and label not in ADC_BACK
        if front:
            dr, dc = ADC_ROWS, ADC_FRONT_COL0 + ADC_FRONT.index(label)
        else:
            dr, dc = 0, ADC_BACK.index(label)
        if self.turned:
            dr, dc = -dr, -dc
        return (self.row + dr, self.col + dc)

    def pins(self):
        out = [(self.pin(p, False), p) for p in ADC_BACK]
        out += [(self.pin(p, True), p) for p in ADC_FRONT]
        return out

    def analog_pins(self):
        """LIN, -, RIN in mm (board coordinates). Off the grid: the RCA
        leads are soldered straight to the module."""
        x, y, w, h = self.outline()
        out = []
        for i, label in enumerate(ADC_ANALOG):
            ax, ay = 2.3, 12.0 + i * P
            if self.turned:
                ax, ay = w - ax, h - ay
            out.append((x + ax, y + ay, label))
        return out

    def outline(self):
        x, y = L.hx(self.col), L.hy(self.row)
        fx, fy = ADC_FIRST
        if self.turned:
            return (x + fx - ADC_W, y + fy - ADC_H, ADC_W, ADC_H)
        return (x - fx, y - fy, ADC_W, ADC_H)


# --------------------------------------------------------------------------
# Layout (filled in below)
# --------------------------------------------------------------------------
ADCS = []
COMPONENTS = []     # (ref, kind, value, (r, c), (r, c))
WIRES = []          # (net, [(r, c), ...])
BRIDGES = []        # [(r, c), ...] bare tinned wire / solder bridge
OFFBOARD = []       # (label, (r, c)) flying leads


def build_layout():
    # Both modules are turned by 180 degrees: their signal header faces the
    # ESP32 and the analog inputs point to the free right half of the board.
    # Every connection from A to B then runs straight up.
    a = Adc("ADC A", "Aux 1", 20, 11, turned=True)
    b = Adc("ADC B", "Aux 2", 9, 11, turned=True)
    ADCS.extend([a, b])

    e = esp_pin
    # Supply: VSYS -> L1 -> C1 under the ESP32, filtered 5 V up the left side
    COMPONENTS.extend([
        ("L1", "L", "10µH", (25, 3), (25, 7)),
        ("C1", "C", "100µF", (27, 3), (28, 3)),
        ("R1", "R", "33Ω", (23, 7), (22, 7)),
        ("R2", "R", "33Ω", (23, 8), (22, 8)),
    ])
    WIRES.extend([
        ("5V", [e("VSYS"), (25, 3)]),
        ("5VF", [(25, 7), (27, 7), (27, 1), (11, 1), a.pin("+5V")]),
        ("5VF", [a.pin("+5V"), b.pin("+5V")]),
        ("GND", [(28, 3), (28, 4), e("GND", "bot")]),
        ("GND", [e("GND"), a.pin("GND", front=True)]),
        ("GND", [a.pin("GND", front=True), b.pin("GND", front=True)]),
        ("3V3", [e("3V3"), a.pin("3.3V")]),
        ("3V3", [a.pin("3.3"), b.pin("3.3")]),
        ("MCLK", [e("20"), (23, 7)]),
        ("MCLK", [(22, 7), (21, 7), (21, 8), a.pin("SCK")]),
        ("MCLK", [a.pin("SCK"), b.pin("SCK")]),
        ("BCK", [e("21"), (23, 8)]),
        ("BCK", [(22, 8), (22, 10), a.pin("BCK")]),
        ("BCK", [a.pin("BCK"), b.pin("BCK")]),
        ("LRCK", [e("22"), (23, 10), (23, 9), a.pin("LRC")]),
        ("LRCK", [a.pin("LRC"), b.pin("LRC")]),
        ("DA", [a.pin("OUT"), e("23")]),
        ("DB", [b.pin("OUT"), (9, 13), e("26")]),
    ])
    # Mode pins FMY, MD1, MD0 to ground: I2S format, slave mode
    for m in (a, b):
        BRIDGES.append([m.pin("GND", front=True), m.pin("FMY")])


# --------------------------------------------------------------------------
# SVG rendering
# --------------------------------------------------------------------------

def px(v):
    return f"{v * S:.1f}"


def pos(rc):
    r, c = rc
    return L.hx(c), L.hy(r)


class Svg:
    def __init__(self):
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=9, anchor="middle", weight="normal", fill="#22302a",
             rotate=None, cls=None):
        tr = f' transform="rotate({rotate} {px(x)} {px(y)})"' if rotate else ""
        self.add(f'<text x="{px(x)}" y="{px(y)}" font-size="{size}" text-anchor="{anchor}" '
                 f'font-weight="{weight}" fill="{fill}"{tr}>{escape(s)}</text>')


def draw_board(svg, solder_side):
    """Board at the current origin. On the solder side the drawing is
    mirrored left/right, as you see the board when you flip it over."""
    g_open = '<g transform="translate({w},0) scale(-1,1)">'.format(w=px(L.BOARD_W)) \
        if solder_side else "<g>"
    svg.add(g_open)
    svg.add(f'<rect x="0" y="0" width="{px(L.BOARD_W)}" height="{px(L.BOARD_H)}" rx="6" '
            f'fill="#e3c98f" stroke="#b5935a" stroke-width="1.5"/>')
    for hx_, hy_ in L.BOARD_HOLES:
        svg.add(f'<circle cx="{px(hx_)}" cy="{px(hy_)}" r="{px(1.6)}" fill="#fff" '
                f'stroke="#b5935a"/>')
    used = set()
    for a in ADCS:
        used |= {rc for rc, _ in a.pins()}
    used |= {esp_pin(n) for n in ESP_PINS_TOP} | {esp_pin(n, "bot") for n in ESP_PINS_BOT}
    for comp in COMPONENTS:
        used |= {comp[3], comp[4]}
    for r in range(L.ROWS):
        for c in range(L.COLS):
            x, y = L.hx(c), L.hy(r)
            fill = "#c8803e" if (r, c) in used else "#d5a56a"
            svg.add(f'<circle cx="{px(x)}" cy="{px(y)}" r="{px(0.85)}" fill="{fill}"/>'
                    f'<circle cx="{px(x)}" cy="{px(y)}" r="{px(0.42)}" fill="#fbf6ea"/>')
    if solder_side:
        draw_wires(svg)
    else:
        draw_components_side(svg)
    svg.add("</g>")


def poly(svg, pts, color, width, extra=""):
    d = " ".join(f"{px(x)},{px(y)}" for x, y in pts)
    svg.add(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
            f'stroke-linecap="round" stroke-linejoin="round"{extra}/>')


def seg_cross(p1, p2, q1, q2):
    """Crossing point of two segments, ignoring shared end points."""
    (x1, y1), (x2, y2), (x3, y3), (x4, y4) = p1, p2, q1, q2
    d = (x2 - x1) * (y4 - y3) - (y2 - y1) * (x4 - x3)
    if abs(d) < 1e-9:
        return None
    t = ((x3 - x1) * (y4 - y3) - (y3 - y1) * (x4 - x3)) / d
    u = ((x3 - x1) * (y2 - y1) - (y3 - y1) * (x2 - x1)) / d
    if 1e-6 < t < 1 - 1e-6 and 1e-6 < u < 1 - 1e-6:
        return x1 + t * (x2 - x1), y1 + t * (y2 - y1)
    return None


def crossings(i):
    """Points where wire i crosses an earlier wire of another net."""
    net, pts = WIRES[i]
    out = []
    for net2, pts2 in WIRES[:i]:
        if net2 == net:
            continue
        for a, b in zip(pts, pts[1:]):
            for c, d in zip(pts2, pts2[1:]):
                x = seg_cross(pos(a), pos(b), pos(c), pos(d))
                if x:
                    out.append(x)
    return out


def draw_wires(svg):
    for ref, kind, value, a, b in COMPONENTS:
        if kind == "R":
            (x1, y1), (x2, y2) = pos(a), pos(b)
            cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
            vertical = abs(x1 - x2) < 0.1
            w, h = (1.6, 3.2) if vertical else (3.2, 1.6)
            svg.add(f'<rect x="{px(cx - w / 2)}" y="{px(cy - h / 2)}" width="{px(w)}" '
                    f'height="{px(h)}" fill="#1f2933" stroke="#c0c7cf" stroke-width="1"/>')
            # Label inside the body (un-mirrored, reading upwards)
            svg.add(f'<g transform="translate({px(cx)},{px(cy)}) scale(-1,1) rotate(-90)">'
                    f'<text x="0" y="3" font-size="8.5" text-anchor="middle" '
                    f'font-weight="bold" fill="#ffffff">{ref}</text></g>')
    for pts in BRIDGES:
        poly(svg, [pos(p) for p in pts], "#8d99a6", 6.5)
        poly(svg, [pos(p) for p in pts], "#c9d1d9", 3.5)
    for i, (net, pts) in enumerate(WIRES):
        color = NETS[net][0]
        xy = [pos(p) for p in pts]
        for x, y in crossings(i):       # this wire jumps over the earlier one
            svg.add(f'<circle cx="{px(x)}" cy="{px(y)}" r="5.5" fill="#fbf6ea"/>')
        poly(svg, xy, "#fbf6ea", 6)
        poly(svg, xy, color, 3.6)
    for net, pts in WIRES:
        for p in (pts[0], pts[-1]):
            x, y = pos(p)
            svg.add(f'<circle cx="{px(x)}" cy="{px(y)}" r="3.4" fill="{NETS[net][0]}" '
                    f'stroke="#fbf6ea" stroke-width="1"/>')


def pin_label(svg, x, y, label, below, color):
    if len(label) <= 2:
        svg.text(x, y + (3.6 if below else -2.0), label, 8.5, fill=color)
    elif below:
        svg.text(x + 0.9, y + 1.6, label, 8.5, anchor="end", fill=color, rotate=-90)
    else:
        svg.text(x + 0.9, y - 1.6, label, 8.5, anchor="start", fill=color, rotate=-90)


def draw_components_side(svg):
    # Wires shown faintly so the top view still tells the whole story
    for net, pts in WIRES:
        poly(svg, [pos(p) for p in pts], NETS[net][0], 2.2, ' stroke-opacity="0.35"')

    # ESP32 board (outline, raised on headers)
    ex, ey = L.ESP_X0, L.ESP_Y0
    svg.add(f'<rect x="{px(ex)}" y="{px(ey)}" width="{px(L.ESP_LEN)}" height="{px(L.ESP_W)}" '
            f'rx="5" fill="#1b2430" fill-opacity="0.82" stroke="#0b1016" stroke-width="1.5"/>')
    svg.add(f'<rect x="{px(ex + L.RJ45_X0)}" y="{px(ey + L.RJ45_Y0)}" width="{px(L.RJ45_LEN)}" '
            f'height="{px(16.7)}" rx="2" fill="#b9bec4" stroke="#6b7280"/>')
    svg.text(ex + L.RJ45_X0 + L.RJ45_LEN / 2, ey + L.RJ45_Y0 + 9.5, "RJ45 / PoE", 13, weight="bold",
             fill="#1f2933")
    svg.add(f'<rect x="{px(ex - 0.6)}" y="{px(ey + L.USB_YC - 4.5)}" width="{px(7.4)}" '
            f'height="{px(9)}" rx="2" fill="#b9bec4" stroke="#6b7280"/>')
    svg.text(ex + 3.4, ey + L.USB_YC, "USB-C", 10, fill="#1f2933", weight="bold", rotate=-90)
    svg.text(ex + 40, ey + L.ESP_W / 2 + 1.5, "Waveshare ESP32-P4-ETH", 15, weight="bold",
             fill="#e8f6ee")
    for row, names in (("top", ESP_PINS_TOP), ("bot", ESP_PINS_BOT)):
        for i, n in enumerate(names):
            x, y = pos(esp_pin(n, row))
            used = n in ("VSYS", "GND", "3V3", "20", "21", "22", "23", "26")
            svg.add(f'<rect x="{px(x - 1.0)}" y="{px(y - 1.0)}" width="{px(2.0)}" '
                    f'height="{px(2.0)}" fill="{"#f5c84c" if used else "#8a96a3"}"/>')
            pin_label(svg, x, y, n, row == "top", "#e8f6ee" if used else "#9aa5b1")
    # Passive components (L1 and C1 sit under the ESP32: drawn see-through)
    for ref, kind, value, a, b in COMPONENTS:
        if kind == "R":                 # SMD, on the solder side
            continue
        (x1, y1), (x2, y2) = pos(a), pos(b)
        cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
        poly(svg, [(x1, y1), (x2, y2)], "#cbd5e1", 2)
        if kind == "L":
            svg.add(f'<rect x="{px(cx - 3.6)}" y="{px(cy - 1.8)}" width="{px(7.2)}" '
                    f'height="{px(3.6)}" rx="8" fill="#5d7f9a" fill-opacity="0.9" '
                    f'stroke="#e8f6ee" stroke-dasharray="3 2"/>')
            svg.text(cx, cy - 2.6, f"{ref} {value}", 11, weight="bold", fill="#e8f6ee")
        elif kind == "C":
            # lying flat, body pointing to the right
            svg.add(f'<rect x="{px(x1 + 1.2)}" y="{px(y1 - 1.2)}" width="{px(9)}" '
                    f'height="{px(5)}" rx="6" fill="#5d7f9a" fill-opacity="0.9" '
                    f'stroke="#e8f6ee" stroke-dasharray="3 2"/>')
            svg.text(x1 - 1.4, y1 + 0.5, "+", 12, weight="bold", fill="#e8f6ee")
            svg.text(x1 + 11.5, y1 + 2.4, f"{ref} {value}", 11, anchor="start",
                     weight="bold", fill="#e8f6ee")

    # ADC modules
    for a in ADCS:
        x, y, w, h = a.outline()
        svg.add(f'<rect x="{px(x)}" y="{px(y)}" width="{px(w)}" height="{px(h)}" rx="4" '
                f'fill="#5b2a86" fill-opacity="0.80" stroke="#3b1659" stroke-width="1.5"/>')
        for hx_, hy_ in ((2.6, 2.6), (w - 2.6, 2.6), (2.6, h - 2.6), (w - 2.6, h - 2.6)):
            svg.add(f'<circle cx="{px(x + hx_)}" cy="{px(y + hy_)}" r="{px(1.5)}" '
                    f'fill="#e3c98f" stroke="#3b1659"/>')
        svg.text(x + w / 2, y + h / 2 - 1, a.ref, 18, weight="bold", fill="#f3e8ff")
        svg.text(x + w / 2, y + h / 2 + 3.6, a.name, 13, fill="#e9d5ff")
        for rc, label in a.pins():
            px_, py_ = pos(rc)
            svg.add(f'<rect x="{px(px_ - 1.0)}" y="{px(py_ - 1.0)}" width="{px(2.0)}" '
                    f'height="{px(2.0)}" fill="#f5c84c"/>')
            back = label in ADC_BACK and rc == a.pin(label, False)
            pin_label(svg, px_, py_, label, back != a.turned, "#ffffff")
        for ax, ay, label in a.analog_pins():
            svg.add(f'<circle cx="{px(ax)}" cy="{px(ay)}" r="{px(0.9)}" fill="#e5e7eb" '
                    f'stroke="#9ca3af"/>')
            svg.text(ax - 2.0, ay + 1.0, label, 8.5, anchor="end", fill="#ffffff")
        ax, ay, _ = a.analog_pins()[1]
        svg.add(f'<path d="M {px(ax + 1.2)} {px(ay)} H {px(ax + 7)}" stroke="#3b1659" '
                f'stroke-width="2" stroke-dasharray="4 3"/>')
        svg.text(ax + 8, ay - 1.0, f"{a.name} RCA jacks", 12, anchor="start",
                 weight="bold", fill="#3b1659")
        svg.text(ax + 8, ay + 2.8, "L, ground, R (shielded)", 10.5, anchor="start",
                 fill="#3b1659")


NOTES = [
    "ESP32: plug it into two 1 x 20 female headers (pins 40..21 towards the ADCs).",
    "ADCs: solder both 6-pin headers into the board, modules turned by 180° as drawn.",
    "FMY, MD1, MD0 bridged to GND: I2S format, slave mode.",
    "3.3 V enters each module on its signal header; the second 3.3 pin passes it on.",
    "R1, R2: 1206 SMD across two pads on the solder side (or a wire bridge).",
    "L1, C1 sit under the ESP32: lay them flat (max. 8 mm high).",
    "Audio: shielded cable from LIN, –, RIN to the RCA jacks; shield to – only.",
    "Check the pin labels of your ADC modules before soldering.",
]


def legend(svg, x0, y0, col_w):
    def heading(x, y, s):
        svg.text(x, y, s, 16, anchor="start", weight="bold")

    heading(x0, y0, "Wires")
    y = y0 + 5.0
    items = list(NETS.items()) + [("_bridge", ("#8d99a6", "solder bridge / bare wire"))]
    for net, (color, desc) in items:
        w = 7 if net == "_bridge" else 4.5
        svg.add(f'<line x1="{px(x0)}" y1="{px(y)}" x2="{px(x0 + 7)}" y2="{px(y)}" '
                f'stroke="{color}" stroke-width="{w}" stroke-linecap="round"/>')
        svg.text(x0 + 9.5, y + 1.4, desc, 13, anchor="start")
        y += 3.5

    x1 = x0 + col_w
    heading(x1, y0, "Parts")
    y = y0 + 5.0
    for line in [
        "1 x Waveshare ESP32-P4-ETH(-M) with pin headers",
        "2 x CJMCU-1808 (PCM1808) ADC module",
        "1 x perfboard 70 x 90 mm (24 x 32 holes)",
        "2 x female header 1 x 20, 2.54 mm",
        "R1, R2: 33 Ω, 1206 SMD",
        "L1: 10 µH axial inductor",
        "C1: 100 µF / 16 V electrolytic, 5 mm",
        "4 x panel RCA (cinch) jack",
        "4 x M3 x 6 screws for the board",
        "4 x M3 x 10 screws for the lid",
    ]:
        svg.text(x1, y + 1.4, line, 13, anchor="start")
        y += 3.5

    x2 = x1 + col_w
    heading(x2, y0, "Notes")
    y = y0 + 5.0
    for line in NOTES:
        svg.text(x2, y + 1.4, line, 13, anchor="start")
        y += 3.5


def render():
    build_layout()
    gap = 26.0
    W = L.BOARD_W * 2 + gap + 16 + 24
    H = 28 + L.BOARD_H + 54
    svg = Svg()
    svg.add(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {px(W)} {px(H)}" '
            f'width="{px(W)}" height="{px(H)}" font-family="Helvetica, Arial, sans-serif">')
    svg.add('<rect width="100%" height="100%" fill="#fbfaf6"/>')
    svg.text(8, 10, "Rumble Jungle – perfboard", 24, anchor="start", weight="bold")
    svg.text(8, 16.5, "70 x 90 mm perfboard, 2.54 mm grid. Modules on the component side, "
             "all wires on the solder side. Generated by hardware/perfboard.py.", 13,
             anchor="start", fill="#52606d")
    x = 8
    for i, title in enumerate(("Component side (top view)",
                               "Solder side (board flipped over left to right)")):
        svg.add(f'<g transform="translate({px(x)},{px(28)})">')
        svg.text(0, -2.5, title, 15, anchor="start", weight="bold")
        draw_board(svg, solder_side=i == 1)
        svg.add("</g>")
        x += L.BOARD_W + gap
    legend(svg, 8, 28 + L.BOARD_H + 12, 62)
    svg.add("</svg>")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        fh.write("\n".join(svg.parts))
    print("wrote", os.path.relpath(OUT))


if __name__ == "__main__":
    render()
