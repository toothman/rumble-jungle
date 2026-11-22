# SPDX-License-Identifier: MIT
"""
Shared geometry of the Rumble Jungle perfboard (used by perfboard.py and
enclosure.py). Units are mm, origin is the top-left corner of the perfboard
seen from the component side, x to the right, y down (towards the front of
the box). Grid positions are (row, col).
"""

# Standard 70 x 90 mm perfboard, 24 x 32 holes at 2.54 mm
PITCH = 2.54
BOARD_W, BOARD_H = 70.0, 90.0
COLS, ROWS = 24, 32
GRID_X0 = (BOARD_W - (COLS - 1) * PITCH) / 2
GRID_Y0 = (BOARD_H - (ROWS - 1) * PITCH) / 2
PERFBOARD_T = 1.6
HOLE_INSET = 3.0                    # corner mounting holes (drill 3.2 mm if missing)
BOARD_HOLES = [(HOLE_INSET, HOLE_INSET), (BOARD_W - HOLE_INSET, HOLE_INSET),
               (HOLE_INSET, BOARD_H - HOLE_INSET), (BOARD_W - HOLE_INSET, BOARD_H - HOLE_INSET)]


def hx(col):
    return GRID_X0 + col * PITCH


def hy(row):
    return GRID_Y0 + row * PITCH


# Waveshare ESP32-P4-ETH: 78 x 21 mm, 2 x 20 pins, rows 17.78 mm apart.
# It sits rotated so that the RJ45 points right and the pin 21..40 row faces
# the ADCs. Pin 40 is 4.67 mm from the USB-C edge.
ESP_LEN, ESP_W, ESP_T = 78.0, 21.0, 1.6
ESP_STACK = 8.5 + 2.5               # female header + plastic of the board's male pins
ESP_COL = 2                         # column of pin 40 / pin 1
ESP_ROW_TOP = 24                    # row of pins 40..21
ESP_ROW_BOT = ESP_ROW_TOP + 7       # row of pins 1..20
ESP_X0 = hx(ESP_COL) - 4.67         # USB-C edge
ESP_Y0 = hy(ESP_ROW_TOP) - 1.61     # long edge on the pin 21..40 side
RJ45_X0, RJ45_LEN = ESP_LEN - 22.0, 22.0
RJ45_Y0 = 1.61                      # RJ45 body from the pin 21..40 edge
USB_YC = 11.7                       # USB-C centre from the pin 21..40 edge
