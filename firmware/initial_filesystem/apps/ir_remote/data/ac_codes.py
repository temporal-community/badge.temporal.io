"""AC remote presets for the IR Playground.

AC remotes are stateful \u2014 each press transmits the full target state
(mode + temp + fan + swing), not button deltas. The library handles
this Flipper-style: there are 6 named state slots, and the user records
each one from a real remote. We ship a few starter presets generated
from the well-known Coolix protocol so users without their physical
remote can still control any Coolix-family AC (Beko, Midea, Tristar,
Goodweather, Lennox \u2014 ~30 cheap brands all share the same wire).

Schema mirrors the codebook:
  VENDORS = ((vendor_name, {state_label: ('raw', bytes, carrier_hz)}), ...)

Slot names match the Flipper Zero AC universal remote:
  Off / Dehumid / Cool-Hi / Cool-Lo / Heat-Hi / Heat-Lo
"""

import struct


# ── Coolix protocol synthesizer ─────────────────────────────────────────────

# Reference: https://github.com/crankyoldgit/IRremoteESP8266 (ir_Coolix.h)
# Carrier: 38 kHz, 50% duty.  Frame layout:
#   Header  : 4480 us mark + 4480 us space
#   24 data bits, MSB first, encoded twice:
#     first the bit, then its 1's-complement (so bit + ~bit per pair)
#   Each bit = 560 us mark + (560 us | 1680 us) space (0 or 1)
#   Footer  : 560 us mark + 5040 us space
# The whole frame (header + bits + footer) is repeated 3 times for
# robustness.  Receivers latch on the matching pair.

_HDR_MARK = 4480
_HDR_SPACE = 4480
_BIT_MARK = 560
_ZERO_SPACE = 560
_ONE_SPACE = 1680
_FOOT_SPACE = 5040
_REPEATS = 3


def _append_pair(out, mark, space):
    out += struct.pack("<HH", mark, space)


def _coolix_frame(value24, out):
    """Append one (header + 48 logical bits + footer) frame."""
    _append_pair(out, _HDR_MARK, _HDR_SPACE)
    # MSB-first: walk bits 23..0
    for i in range(23, -1, -1):
        bit = (value24 >> i) & 1
        # bit
        _append_pair(out, _BIT_MARK, _ONE_SPACE if bit else _ZERO_SPACE)
        # ~bit
        _append_pair(out, _BIT_MARK, _ZERO_SPACE if bit else _ONE_SPACE)
    _append_pair(out, _BIT_MARK, _FOOT_SPACE)


def _build_coolix(value24):
    out = bytearray()
    for _ in range(_REPEATS):
        _coolix_frame(value24, out)
    # Strip the very last footer space so the buffer doesn't end with an
    # idle gap that the RMT driver would otherwise attempt to encode.
    if len(out) >= 4:
        # Replace the last (mark, space) with (mark, 0) to terminate.
        last_mark, _ = struct.unpack_from("<HH", out, len(out) - 4)
        struct.pack_into("<HH", out, len(out) - 4, last_mark, 0)
    return bytes(out)


# ── Common Coolix state codes ──────────────────────────────────────────────
# Sourced from IRremoteESP8266 examples + community remotes. All 24-bit.

_COOLIX_OFF      = 0xB27BE0
_COOLIX_COOL_22F = 0xB2BF60   # cool, 22 C, fan auto
_COOLIX_COOL_18M = 0xB21FE0   # cool, 18 C, fan medium
_COOLIX_DRY_AUTO = 0xB21F40   # dehumidify auto
_COOLIX_HEAT_22  = 0xB2BFA0   # heat, 22 C, fan auto
_COOLIX_HEAT_28L = 0xB29FE0   # heat, 28 C, fan low


def _slot(value24):
    return ("raw", _build_coolix(value24), 38000)


# ── Vendor table ───────────────────────────────────────────────────────────


VENDORS = (
    ("Coolix (Beko/Midea)", {
        "Off":      _slot(_COOLIX_OFF),
        "Dehumid":  _slot(_COOLIX_DRY_AUTO),
        "Cool-Hi":  _slot(_COOLIX_COOL_22F),
        "Cool-Lo":  _slot(_COOLIX_COOL_18M),
        "Heat-Hi":  _slot(_COOLIX_HEAT_22),
        "Heat-Lo":  _slot(_COOLIX_HEAT_28L),
    }),
    # Other AC families (Daikin, Mitsubishi, Fujitsu, Gree) need their
    # own bespoke synthesizers \u2014 not implemented yet. Use the "Custom"
    # workflow inside the AC tile to record each state from your remote.
)
