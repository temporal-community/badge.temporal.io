"""IR Playground app icon — 12x12 XBM byte pairs (LSB-first per row).

Visual: a stylized remote with a dotted IR cone radiating from the top.
Bytes match AppIcons::irPlayground in firmware/src/ui/AppIcons.h.

Row guides (left byte = bits 0..7, right byte = bits 8..11):
  ........#..#   IR sparkle
  ..#..#......
  ........#..#
  ..####......   remote top
  .######.....
  .##..##.....
  .######.....
  .######.....
  .#.##.#.....
  .######.....
  .######.....
  ..####......   remote bottom
"""

WIDTH = 12
HEIGHT = 12

DATA = (
    0b00000000, 0b00001001,
    0b00100100, 0b00000000,
    0b00000000, 0b00001001,
    0b00111100, 0b00000000,
    0b01111110, 0b00000000,
    0b01100110, 0b00000000,
    0b01111110, 0b00000000,
    0b01111110, 0b00000000,
    0b01011010, 0b00000000,
    0b01111110, 0b00000000,
    0b01111110, 0b00000000,
    0b00111100, 0b00000000,
)
