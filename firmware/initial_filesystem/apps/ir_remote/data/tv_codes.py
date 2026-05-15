"""Curated NEC TV codes for the IR Playground universal remote.

Format mirrors Flipper Zero's TV universal remote slots:
  Power, Vol+, Vol-, Mute, Ch+, Ch-

Schema:
  VENDORS = ((vendor_name, {button: ('nec', addr, cmd, repeats)}), ...)

These codes were chosen for high coverage of widely-deployed brands.
Sony / RC5 / RC6 vendors deliberately not represented here \u2014 those need
raw-symbol playback because the protocol differs from NEC.
"""

VENDORS = (
    ("Samsung", {
        "Power":  ("nec", 0x07, 0x02, 1),
        "Vol+":   ("nec", 0x07, 0x07, 1),
        "Vol-":   ("nec", 0x07, 0x0B, 1),
        "Mute":   ("nec", 0x07, 0x0F, 1),
        "Ch+":    ("nec", 0x07, 0x12, 1),
        "Ch-":    ("nec", 0x07, 0x10, 1),
    }),
    ("LG", {
        "Power":  ("nec", 0x04, 0x08, 1),
        "Vol+":   ("nec", 0x04, 0x02, 1),
        "Vol-":   ("nec", 0x04, 0x03, 1),
        "Mute":   ("nec", 0x04, 0x09, 1),
        "Ch+":    ("nec", 0x04, 0x00, 1),
        "Ch-":    ("nec", 0x04, 0x01, 1),
    }),
    ("Vizio", {
        "Power":  ("nec", 0x14, 0x08, 1),
        "Vol+":   ("nec", 0x14, 0x02, 1),
        "Vol-":   ("nec", 0x14, 0x03, 1),
        "Mute":   ("nec", 0x14, 0x09, 1),
        "Ch+":    ("nec", 0x14, 0x00, 1),
        "Ch-":    ("nec", 0x14, 0x01, 1),
    }),
    ("Hisense", {
        "Power":  ("nec", 0x40, 0x14, 1),
        "Vol+":   ("nec", 0x40, 0x10, 1),
        "Vol-":   ("nec", 0x40, 0x11, 1),
        "Mute":   ("nec", 0x40, 0x0F, 1),
        "Ch+":    ("nec", 0x40, 0x18, 1),
        "Ch-":    ("nec", 0x40, 0x19, 1),
    }),
    ("TCL", {
        "Power":  ("nec", 0x40, 0x14, 1),
        "Vol+":   ("nec", 0x40, 0x10, 1),
        "Vol-":   ("nec", 0x40, 0x11, 1),
        "Mute":   ("nec", 0x40, 0x0F, 1),
        "Ch+":    ("nec", 0x40, 0x18, 1),
        "Ch-":    ("nec", 0x40, 0x19, 1),
    }),
    ("Sharp", {
        "Power":  ("nec", 0x10, 0x16, 1),
        "Vol+":   ("nec", 0x10, 0x32, 1),
        "Vol-":   ("nec", 0x10, 0x33, 1),
        "Mute":   ("nec", 0x10, 0x39, 1),
        "Ch+":    ("nec", 0x10, 0x35, 1),
        "Ch-":    ("nec", 0x10, 0x34, 1),
    }),
    ("Toshiba", {
        "Power":  ("nec", 0x40, 0x12, 1),
        "Vol+":   ("nec", 0x40, 0x1A, 1),
        "Vol-":   ("nec", 0x40, 0x1E, 1),
        "Mute":   ("nec", 0x40, 0x10, 1),
        "Ch+":    ("nec", 0x40, 0x1B, 1),
        "Ch-":    ("nec", 0x40, 0x1F, 1),
    }),
    ("Panasonic", {
        "Power":  ("nec", 0x40, 0x3D, 1),
        "Vol+":   ("nec", 0x40, 0x20, 1),
        "Vol-":   ("nec", 0x40, 0x21, 1),
        "Mute":   ("nec", 0x40, 0x32, 1),
        "Ch+":    ("nec", 0x40, 0x22, 1),
        "Ch-":    ("nec", 0x40, 0x23, 1),
    }),
    ("Philips", {
        "Power":  ("nec", 0x00, 0x0C, 1),
        "Vol+":   ("nec", 0x00, 0x10, 1),
        "Vol-":   ("nec", 0x00, 0x11, 1),
        "Mute":   ("nec", 0x00, 0x0D, 1),
        "Ch+":    ("nec", 0x00, 0x20, 1),
        "Ch-":    ("nec", 0x00, 0x21, 1),
    }),
    ("RCA", {
        "Power":  ("nec", 0x80, 0x12, 1),
        "Vol+":   ("nec", 0x80, 0x16, 1),
        "Vol-":   ("nec", 0x80, 0x17, 1),
        "Mute":   ("nec", 0x80, 0x18, 1),
        "Ch+":    ("nec", 0x80, 0x14, 1),
        "Ch-":    ("nec", 0x80, 0x15, 1),
    }),
)
