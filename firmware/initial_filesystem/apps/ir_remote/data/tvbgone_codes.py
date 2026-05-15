"""Ordered list of NEC TV "power off" codes for the IR Playground.

Sony / RC5 / RC6 TVs are not reachable through this list — those need
raw-symbol playback. The order roughly mirrors Mitch Altman's original
TV-B-Gone NA priority: most common brands first.
"""

# (vendor, addr, cmd)
CODES = (
    ("Samsung",   0x07, 0x02),
    ("Samsung",   0x07, 0x40),
    ("LG",        0x04, 0x08),
    ("Vizio",     0x14, 0x08),
    ("NEC",       0x00, 0x12),
    ("Onkyo",     0xD2, 0x12),
    ("Pioneer",   0xA5, 0x38),
    ("Toshiba",   0x40, 0x12),
    ("Toshiba",   0x40, 0x15),
    ("Philips",   0x00, 0x0C),
    ("Sharp",     0x10, 0x16),
    ("Sharp",     0x10, 0x59),
    ("Magnavox",  0x80, 0x10),
    ("Sanyo",     0x18, 0x12),
    ("Hitachi",   0x10, 0x15),
    ("JVC",       0x03, 0x37),
    ("Insignia",  0x40, 0x12),
    ("RCA",       0x80, 0x12),
    ("Westinghse",0x14, 0x08),
    ("Element",   0x14, 0x08),
    ("Sceptre",   0x14, 0x08),
    ("Hisense",   0x40, 0x14),
    ("TCL",       0x40, 0x14),
    ("Panasonic", 0x40, 0x3D),
    ("Panasonic", 0x40, 0x12),
    ("Mitsubishi",0x23, 0xC0),
    ("Sylvania",  0x80, 0x10),
    ("Polaroid",  0x14, 0x08),
    ("Apex",      0x80, 0x12),
    ("Akai",      0x40, 0x12),
)
