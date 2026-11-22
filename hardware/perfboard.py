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
    "BCK":  ("#9b51e0", "BCK  GPIO26 -> BCK"),
    "LRCK": ("#27ae60", "LRCK  GPIO22 -> LRC"),
    "DA":   ("#00a3a3", "DATA A  OUT (ADC A) -> GPIO23"),
    "DB":   ("#e0569b", "DATA B  OUT (ADC B) -> GPIO47"),
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


# CJMCU-1808 module (measured): 33.5 x 27 mm, mounting holes 29 x 22 mm.
# It lies with its long side horizontal, turned so that the signal header
# is at the bottom (towards the ESP32) and the analog inputs point right.
# Both headers sit on the 2.54 mm grid, 10 pitches apart, in the same
# columns. Positions in mm from the module's top-left corner.
ADC_W, ADC_H = 33.5, 27.0
ADC_HOLES = (29.0, 22.0)
ADC_PIN0 = (11.6, 0.9)                       # first control pin (+5V)
ADC_SIGNAL = ["3.3V", "GND", "SCK", "LRC", "OUT", "BCK"]   # bottom row, left to right
ADC_CONTROL = ["+5V", "3.3", "GND", "MD0", "MD1", "FMY"]   # top row, left to right
ADC_ROWS = 10
ADC_ANALOG = [("RIN", 32.6, 10.9), ("-", 32.6, 13.4), ("LIN", 32.6, 16.0)]


class Adc:
    """An ADC module whose signal header starts at (row, col)."""

    def __init__(self, ref, name, row, col):
        self.ref, self.name, self.row, self.col = ref, name, row, col

    def pin(self, label, control=False):
        if control:
            return (self.row - ADC_ROWS, self.col + ADC_CONTROL.index(label))
        return (self.row, self.col + ADC_SIGNAL.index(label))

    def pins(self):
        return ([(self.pin(p), p, True) for p in ADC_SIGNAL] +
                [(self.pin(p, True), p, False) for p in ADC_CONTROL])

    def outline(self):
        x = L.hx(self.col) - ADC_PIN0[0]
        y = L.hy(self.row - ADC_ROWS) - ADC_PIN0[1]
        return (x, y, ADC_W, ADC_H)

    def analog_pins(self):
        """RIN, -, LIN in mm (board coordinates). Not on the grid: the RCA
        leads are soldered straight to the module."""
        x, y, _, _ = self.outline()
        return [(x + ax, y + ay, label) for label, ax, ay in ADC_ANALOG]


# --------------------------------------------------------------------------
# Layout
# --------------------------------------------------------------------------
ADCS = []
COMPONENTS = []     # (ref, kind, value, (r, c), (r, c))
WIRES = []          # (net, [(r, c), ...])
BRIDGES = []        # [(r, c), ...] bare tinned wire / solder bridge


def build_layout():
    # ADC A sits right above the ESP32 with its signal header lined up with
    # the ESP32 pins. ADC B sits above A, shifted right by six columns, so
    # its wires pass beside A's control header. All wires run on the solder
    # side and none of them cross.
    a = Adc("ADC A", "Aux 1", 21, 7)
    b = Adc("ADC B", "Aux 2", 10, 13)
    ADCS.extend([a, b])

    e = esp_pin
    COMPONENTS.extend([
        ("L1", "L", "10µH", (25, 3), (25, 7)),
        ("C1", "C", "100µF", (27, 3), (28, 3)),
        ("R1", "R", "33Ω", (22, 9), (23, 9)),
        ("R2", "R", "33Ω", (22, 13), (23, 13)),
    ])
    WIRES.extend([
        # Supply: VSYS -> L1 -> C1 under the ESP32, filtered 5 V up the left
        ("5V", [e("VSYS"), (25, 3)]),
        ("5VF", [(25, 7), (27, 7), (27, 1), (12, 1), (12, 7), a.pin("+5V", True)]),
        ("GND", [(28, 3), (28, 4), e("GND", "bot")]),
        ("GND", [e("GND"), (13, 4), (13, 9), a.pin("GND", True)]),
        ("3V3", [a.pin("3.3V"), (22, 7), (22, 6), e("3V3")]),
        # ESP32 -> ADC A
        ("MCLK", [a.pin("SCK"), (22, 9)]),
        ("MCLK", [(23, 9), (23, 7), e("20")]),
        ("LRCK", [a.pin("LRC"), e("22")]),
        ("DA", [a.pin("OUT"), e("23")]),
        ("BCK", [a.pin("BCK"), (22, 12), (22, 13)]),
        ("BCK", [(23, 13), e("26")]),
        # ADC A -> ADC B: clocks (staircase, the rightmost wire turns first)
        ("MCLK", [a.pin("SCK"), (14, 9), (14, 15), b.pin("SCK")]),
        ("LRCK", [a.pin("LRC"), (16, 10), (16, 16), b.pin("LRC")]),
        ("BCK", [a.pin("BCK"), (18, 12), (18, 18), b.pin("BCK")]),
        # ADC A -> ADC B: supply, from A's control header to B's
        ("5VF", [a.pin("+5V", True), (7, 7), (7, 13), b.pin("+5V", True)]),
        ("3V3", [a.pin("3.3", True), (8, 8), (8, 14), b.pin("3.3", True)]),
        ("GND", [a.pin("GND", True), (9, 9), (9, 15), b.pin("GND", True)]),
        # ADC B -> ESP32
        ("DB", [b.pin("OUT"), (9, 17), (9, 20), e("47")]),
    ])
    # Mode pins MD0, MD1, FMY to ground: slave mode, I2S format
    for m in (a, b):
        BRIDGES.append([m.pin("GND", True), m.pin("FMY", True)])


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
        used |= {rc for rc, _, _ in a.pins()}
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
            used = n in ("VSYS", "GND", "3V3", "20", "22", "23", "26", "47")
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
        mx, my = (ADC_W - ADC_HOLES[0]) / 2, (ADC_H - ADC_HOLES[1]) / 2
        for hx_, hy_ in ((mx, my), (w - mx, my), (mx, h - my), (w - mx, h - my)):
            svg.add(f'<circle cx="{px(x + hx_)}" cy="{px(y + hy_)}" r="{px(1.5)}" '
                    f'fill="#e3c98f" stroke="#3b1659"/>')
        svg.text(x + w / 2, y + h / 2 - 1, a.ref, 18, weight="bold", fill="#f3e8ff")
        svg.text(x + w / 2, y + h / 2 + 3.6, a.name, 13, fill="#e9d5ff")
        for rc, label, signal in a.pins():
            px_, py_ = pos(rc)
            svg.add(f'<rect x="{px(px_ - 1.0)}" y="{px(py_ - 1.0)}" width="{px(2.0)}" '
                    f'height="{px(2.0)}" fill="#f5c84c"/>')
            pin_label(svg, px_, py_, label, not signal, "#ffffff")
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
    "ADCs: solder both 6-pin headers into the board, analog inputs to the right.",
    "MD0, MD1, FMY bridged to GND: slave mode, I2S format.",
    "Each module passes 3.3 V from its 3.3V pin to its 3.3 pin (check with a meter).",
    "R1, R2: 1206 SMD across two pads on the solder side (or a wire bridge).",
    "L1, C1 sit under the ESP32: lay them flat (max. 8 mm high).",
    "Audio: shielded cable from RIN, –, LIN to the RCA jacks; shield to – only.",
    "Needs firmware 1.1 or newer (BCK on GPIO26, DATA B on GPIO47).",
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
