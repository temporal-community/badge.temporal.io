"""Curated NEC projector codes.

Slots match Flipper Zero's projector universal remote:
  Power, Vol+, Vol-, Mute
"""

VENDORS = (
    ("Epson", {
        "Power":  ("nec", 0x83, 0x55, 1),
        "Vol+":   ("nec", 0x83, 0x56, 1),
        "Vol-":   ("nec", 0x83, 0x57, 1),
        "Mute":   ("nec", 0x83, 0x52, 1),
    }),
    ("BenQ", {
        "Power":  ("nec", 0x00, 0x83, 1),
        "Vol+":   ("nec", 0x00, 0x82, 1),
        "Vol-":   ("nec", 0x00, 0x80, 1),
        "Mute":   ("nec", 0x00, 0x84, 1),
    }),
    ("Optoma", {
        "Power":  ("nec", 0x32, 0x02, 1),
        "Vol+":   ("nec", 0x32, 0x09, 1),
        "Vol-":   ("nec", 0x32, 0x0A, 1),
        "Mute":   ("nec", 0x32, 0x52, 1),
    }),
    ("NEC Proj", {
        "Power":  ("nec", 0x00, 0x12, 1),
        "Vol+":   ("nec", 0x00, 0x16, 1),
        "Vol-":   ("nec", 0x00, 0x17, 1),
        "Mute":   ("nec", 0x00, 0x14, 1),
    }),
    ("ViewSonic", {
        "Power":  ("nec", 0x14, 0x03, 1),
        "Vol+":   ("nec", 0x14, 0x09, 1),
        "Vol-":   ("nec", 0x14, 0x08, 1),
        "Mute":   ("nec", 0x14, 0x05, 1),
    }),
)
