"""Curated NEC audio receiver / amplifier codes.

Slots match Flipper Zero's audio universal remote:
  Power, Play, Pause, Vol+, Vol-, Next, Prev, Mute
"""

VENDORS = (
    ("Onkyo", {
        "Power":  ("nec", 0xD2, 0x12, 1),
        "Play":   ("nec", 0xD2, 0x33, 1),
        "Pause":  ("nec", 0xD2, 0x34, 1),
        "Vol+":   ("nec", 0xD2, 0x40, 1),
        "Vol-":   ("nec", 0xD2, 0x41, 1),
        "Next":   ("nec", 0xD2, 0x36, 1),
        "Prev":   ("nec", 0xD2, 0x37, 1),
        "Mute":   ("nec", 0xD2, 0x42, 1),
    }),
    ("Pioneer", {
        "Power":  ("nec", 0xA5, 0x38, 1),
        "Play":   ("nec", 0xA5, 0x18, 1),
        "Pause":  ("nec", 0xA5, 0x19, 1),
        "Vol+":   ("nec", 0xA5, 0x0A, 1),
        "Vol-":   ("nec", 0xA5, 0x0B, 1),
        "Next":   ("nec", 0xA5, 0x14, 1),
        "Prev":   ("nec", 0xA5, 0x15, 1),
        "Mute":   ("nec", 0xA5, 0x49, 1),
    }),
    ("Yamaha", {
        "Power":  ("nec", 0x7E, 0x1E, 1),
        "Play":   ("nec", 0x7E, 0xE2, 1),
        "Pause":  ("nec", 0x7E, 0xE3, 1),
        "Vol+":   ("nec", 0x7E, 0x1A, 1),
        "Vol-":   ("nec", 0x7E, 0x1B, 1),
        "Next":   ("nec", 0x7E, 0xE0, 1),
        "Prev":   ("nec", 0x7E, 0xE1, 1),
        "Mute":   ("nec", 0x7E, 0x1C, 1),
    }),
    ("Bose", {
        "Power":  ("nec", 0x88, 0x40, 1),
        "Play":   ("nec", 0x88, 0x47, 1),
        "Pause":  ("nec", 0x88, 0x48, 1),
        "Vol+":   ("nec", 0x88, 0x42, 1),
        "Vol-":   ("nec", 0x88, 0x43, 1),
        "Next":   ("nec", 0x88, 0x49, 1),
        "Prev":   ("nec", 0x88, 0x4A, 1),
        "Mute":   ("nec", 0x88, 0x44, 1),
    }),
    ("Denon", {
        "Power":  ("nec", 0x82, 0x3D, 1),
        "Play":   ("nec", 0x82, 0x32, 1),
        "Pause":  ("nec", 0x82, 0x33, 1),
        "Vol+":   ("nec", 0x82, 0x10, 1),
        "Vol-":   ("nec", 0x82, 0x11, 1),
        "Next":   ("nec", 0x82, 0x30, 1),
        "Prev":   ("nec", 0x82, 0x31, 1),
        "Mute":   ("nec", 0x82, 0x12, 1),
    }),
)
