#!/usr/bin/env python3
"""
render_button_glyphs_svg.py — render the badge's 10×10 button-cluster
XBM bitmaps as scalable SVGs for the documentation site.

Inputs are the byte arrays from
firmware/src/ui/ButtonGlyphs.cpp (kYBits / kBBits / kABits / kXBits /
kAllBits / kLRBits / kUDBits). Each byte sequence is XBM byte order:
LSB = leftmost pixel, two bytes per row, 10 px wide × 10 px tall.

Outputs one SVG per glyph into the given output directory. SVGs are
mono (currentColor), 100×100 viewBox so they scale crisply on the
docs site, and use square pixels so the original 10×10 art stays
recognizable.

Usage:
    python3 render_button_glyphs_svg.py [out_dir]

Default out_dir is ../../Jumperless-docs/docs/img/badge-buttons/
relative to this script.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

# ── Glyph data ────────────────────────────────────────────────────────────
# Verbatim from firmware/src/ui/ButtonGlyphs.cpp. If you change them
# there, re-run this script to regenerate the SVGs.

GLYPHS: dict[str, list[int]] = {
    "y": [
        0x30, 0x00, 0x78, 0x00, 0x78, 0x00, 0xB6, 0x01,
        0x49, 0x02, 0x49, 0x02, 0xB6, 0x01, 0x48, 0x00,
        0x48, 0x00, 0x30, 0x00,
    ],
    "b": [
        0x30, 0x00, 0x48, 0x00, 0x48, 0x00, 0xB6, 0x01,
        0xC9, 0x03, 0xC9, 0x03, 0xB6, 0x01, 0x48, 0x00,
        0x48, 0x00, 0x30, 0x00,
    ],
    "a": [
        0x30, 0x00, 0x48, 0x00, 0x48, 0x00, 0xB6, 0x01,
        0x49, 0x02, 0x49, 0x02, 0xB6, 0x01, 0x78, 0x00,
        0x78, 0x00, 0x30, 0x00,
    ],
    "x": [
        0x30, 0x00, 0x48, 0x00, 0x48, 0x00, 0xB6, 0x01,
        0x4F, 0x02, 0x4F, 0x02, 0xB6, 0x01, 0x48, 0x00,
        0x48, 0x00, 0x30, 0x00,
    ],
    "all": [
        0x30, 0x00, 0x78, 0x00, 0x78, 0x00, 0xB6, 0x01,
        0xCF, 0x03, 0xCF, 0x03, 0xB6, 0x01, 0x78, 0x00,
        0x78, 0x00, 0x30, 0x00,
    ],
    "lr": [
        0x30, 0x00, 0x48, 0x00, 0x48, 0x00, 0xB6, 0x01,
        0xCF, 0x03, 0xCF, 0x03, 0xB6, 0x01, 0x48, 0x00,
        0x48, 0x00, 0x30, 0x00,
    ],
    "ud": [
        0x30, 0x00, 0x78, 0x00, 0x78, 0x00, 0xB6, 0x01,
        0x49, 0x02, 0x49, 0x02, 0xB6, 0x01, 0x78, 0x00,
        0x78, 0x00, 0x30, 0x00,
    ],
}

GLYPH_W = 10
GLYPH_H = 10


def xbm_pixels(byts: list[int]) -> list[list[bool]]:
    """Decode XBM bytes into a 10×10 boolean grid. LSB = leftmost px."""
    rows: list[list[bool]] = []
    for y in range(GLYPH_H):
        # Two bytes per row; we only care about the low 10 bits.
        b0 = byts[y * 2]
        b1 = byts[y * 2 + 1]
        word = (b1 << 8) | b0
        row = [bool((word >> x) & 1) for x in range(GLYPH_W)]
        rows.append(row)
    return rows


def to_svg(name: str, byts: list[int], cell: int = 10) -> str:
    """Render glyph as a square-pixel SVG using `currentColor` so the
    docs theme can recolor via CSS."""
    grid = xbm_pixels(byts)
    side = GLYPH_W * cell
    rects: list[str] = []
    for y, row in enumerate(grid):
        # Run-length compress horizontally per row to keep the SVG
        # source small without sacrificing fidelity.
        x = 0
        while x < GLYPH_W:
            if not row[x]:
                x += 1
                continue
            run_start = x
            while x < GLYPH_W and row[x]:
                x += 1
            run_len = x - run_start
            rects.append(
                f'<rect x="{run_start * cell}" y="{y * cell}" '
                f'width="{run_len * cell}" height="{cell}"/>'
            )
    svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {side} {side}" width="32" height="32" '
        f'role="img" aria-label="badge button glyph: {name}" '
        f'fill="currentColor" shape-rendering="crispEdges">\n'
        + "\n".join("  " + r for r in rects)
        + "\n</svg>\n"
    )
    return svg


def main() -> int:
    here = Path(__file__).resolve().parent
    default_out = (here / ".." / ".." / ".." / "Jumperless-docs" /
                   "docs" / "img" / "badge-buttons").resolve()

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "out_dir", nargs="?", default=str(default_out),
        help=f"Output directory (default: {default_out})",
    )
    args = parser.parse_args()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    for name, byts in GLYPHS.items():
        svg = to_svg(name, byts)
        path = out_dir / f"{name}.svg"
        path.write_text(svg, encoding="utf-8")
        print(f"  wrote {path.relative_to(Path.cwd()) if path.is_relative_to(Path.cwd()) else path}")

    print(f"\nGenerated {len(GLYPHS)} SVGs in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
