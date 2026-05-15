#include "FontCatalog.h"

#include <U8g2lib.h>

const char* const kSizeLabels[kSizeCount] = {
    "4px", "6px", "7px", "8px", "9px", "10px", "13px", "15px", "20px", "XL"
};

// [family][size] → U8G2 font pointer.
// Size is a universal scale (0=4px .. 9=XL) independent of font family.
// Where a family lacks a native font at a given size, the closest match is used.
const uint8_t* kFontGrid[kFontGridFamilyCount][kSizeCount] = {
    // 0: Default (generic built-in)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_5x7_tr,
      u8g2_font_5x8_tr, u8g2_font_6x10_tr, u8g2_font_6x12_tr,
      u8g2_font_7x13_tr, u8g2_font_9x15_tr,
      u8g2_font_10x20_tr, u8g2_font_inb24_mr },
    // 1: Profont (clean monospace). Biggest slots now use profont29
    // (~29 px tall) for big-nametag use cases; slot 7 still lands on
    // profont22 so cycle-through-sizes doesn't jump straight from 17
    // to 29.
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_profont10_tr,
      u8g2_font_profont11_tr, u8g2_font_profont12_tr, u8g2_font_profont15_tr,
      u8g2_font_profont17_tr, u8g2_font_profont22_tr,
      u8g2_font_profont29_tr, u8g2_font_profont29_tr },
    // 2: Fixed (X11 fixed-width)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_5x7_tr,
      u8g2_font_5x8_tr, u8g2_font_6x10_tr, u8g2_font_6x12_tr,
      u8g2_font_8x13_tr, u8g2_font_9x15_tr,
      u8g2_font_10x20_tr, u8g2_font_10x20_tr },
    // 3: Courier (serif monospace)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_courR08_tr,
      u8g2_font_courR08_tr, u8g2_font_courR10_tr, u8g2_font_courR10_tr,
      u8g2_font_courR12_tr, u8g2_font_courR14_tr,
      u8g2_font_courR18_tr, u8g2_font_courR24_tr },
    // 4: Helvetica (sans-serif)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_helvR08_tr,
      u8g2_font_helvR08_tr, u8g2_font_helvR10_tr, u8g2_font_helvR10_tr,
      u8g2_font_helvR12_tr, u8g2_font_helvR14_tr,
      u8g2_font_helvR18_tr, u8g2_font_helvR24_tr },
    // 5: NCenR (New Century serif)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_ncenR08_tr,
      u8g2_font_ncenR08_tr, u8g2_font_ncenR10_tr, u8g2_font_ncenR10_tr,
      u8g2_font_ncenR12_tr, u8g2_font_ncenR14_tr,
      u8g2_font_ncenR18_tr, u8g2_font_ncenR18_tr },
    // 6: Boutique (pixel art bitmap)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_boutique_bitmap_7x7_tr,
      u8g2_font_boutique_bitmap_7x7_tr, u8g2_font_boutique_bitmap_9x9_tr,
      u8g2_font_boutique_bitmap_9x9_bold_tr, u8g2_font_7x13_tr,
      u8g2_font_9x15_tr, u8g2_font_10x20_tr, u8g2_font_inb24_mr },
    // 7: Terminus (UW ttyp0 monospace — smallest native is 8px cap =
    // t0_11). Slots 8-9 jump straight to t0_30 / t0_30 bold because
    // t0_22's `_tr` variant doesn't ship in u8g2 (numbers-only `_tn`
    // is the only 22-px option, unusable for names). The 17→30 jump
    // means drawNametag's tier ladder skips 22.
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_5x7_tr,
      u8g2_font_5x8_tr, u8g2_font_t0_11_tr, u8g2_font_t0_12_tr,
      u8g2_font_t0_14_tr, u8g2_font_t0_17_tr,
      u8g2_font_t0_30_tr, u8g2_font_t0_30b_tr },
    // 8: Spleen (clean monospace, 5x8→6x12→8x16→12x24→16x32→32x64).
    // Slot ladder now gives a distinct Spleen size at every step from
    // slot 3 up, so drawNametag has plenty of intermediate fallbacks
    // when a long name forces a downshift. Slot 9 is 32x64 (literally
    // fills the 64-px screen height) — not useful for multi-line
    // nametags but handy for single-char/single-word displays, so it
    // lives in the grid rather than only in the flat catalog.
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_spleen5x8_mr,
      u8g2_font_spleen5x8_mr, u8g2_font_spleen6x12_mr, u8g2_font_spleen6x12_mr,
      u8g2_font_spleen8x16_mr, u8g2_font_spleen12x24_mr,
      u8g2_font_spleen16x32_mr, u8g2_font_spleen32x64_mr },
    // 9: Times (Adobe serif proportional, same sizes as Courier/Helv/NCen)
    { u8g2_font_u8glib_4_tr, u8g2_font_4x6_tr, u8g2_font_timR08_tr,
      u8g2_font_timR08_tr, u8g2_font_timR10_tr, u8g2_font_timR10_tr,
      u8g2_font_timR12_tr, u8g2_font_timR14_tr,
      u8g2_font_timR18_tr, u8g2_font_timR24_tr },
};

// Flat catalog kept for setFont(name) API and MicroPython bridge.
const ReplayFont kReplayFonts[] = {
    { u8g2_font_u8glib_4_tr,                 "4px"            },
    { u8g2_font_4x6_tr,                      "6px"            },
    { u8g2_font_5x7_tr,                      "5x7"            },
    { u8g2_font_5x8_tr,                      "5x8"            },
    { u8g2_font_smallsimple_tr,              "Smallsimple"    },
    { u8g2_font_6x10_tr,                     "6x10"           },
    { u8g2_font_6x12_tr,                     "6x12"           },
    { u8g2_font_7x13_tr,                     "7x13"           },
    { u8g2_font_8x13_tr,                     "8x13"           },
    { u8g2_font_9x15_tr,                     "9x15"           },
    { u8g2_font_10x20_tr,                    "10x20"          },
    { u8g2_font_inb24_mr,                    "inb24"          },
    { u8g2_font_profont10_tr,                "Profont 10"     },
    { u8g2_font_profont11_tr,                "Profont 11"     },
    { u8g2_font_profont12_tr,                "Profont 12"     },
    { u8g2_font_profont15_tr,                "Profont 15"     },
    { u8g2_font_profont17_tr,                "Profont 17"     },
    { u8g2_font_profont22_tr,                "Profont 22"     },
    { u8g2_font_profont29_tr,                "Profont 29"     },
    { u8g2_font_courR08_tr,                  "Courier 08"     },
    { u8g2_font_courR10_tr,                  "Courier 10"     },
    { u8g2_font_courR12_tr,                  "Courier 12"     },
    { u8g2_font_courR14_tr,                  "Courier 14"     },
    { u8g2_font_courR18_tr,                  "Courier 18"     },
    { u8g2_font_courR24_tr,                  "Courier 24"     },
    { u8g2_font_helvR08_tr,                  "Helv 08"        },
    { u8g2_font_helvR10_tr,                  "Helv 10"        },
    { u8g2_font_helvR12_tr,                  "Helv 12"        },
    { u8g2_font_helvR14_tr,                  "Helv 14"        },
    { u8g2_font_helvR18_tr,                  "Helv 18"        },
    { u8g2_font_helvR24_tr,                  "Helv 24"        },
    { u8g2_font_ncenR08_tr,                  "NCen 08"        },
    { u8g2_font_ncenR10_tr,                  "NCen 10"        },
    { u8g2_font_ncenR12_tr,                  "NCen 12"        },
    { u8g2_font_ncenR14_tr,                  "NCen 14"        },
    { u8g2_font_ncenR18_tr,                  "NCen 18"        },
    { u8g2_font_boutique_bitmap_7x7_tr,      "Boutique 7x7"   },
    { u8g2_font_boutique_bitmap_9x9_tr,      "Boutique 9x9"   },
    { u8g2_font_boutique_bitmap_9x9_bold_tr, "Boutique 9x9B"  },
    { u8g2_font_t0_11_tr,                    "Terminus 11"    },
    { u8g2_font_t0_12_tr,                    "Terminus 12"    },
    { u8g2_font_t0_14_tr,                    "Terminus 14"    },
    { u8g2_font_t0_17_tr,                    "Terminus 17"    },
    { u8g2_font_t0_30_tr,                    "Terminus 30"    },
    { u8g2_font_t0_30b_tr,                   "Terminus 30B"   },
    { u8g2_font_spleen5x8_mr,                "Spleen 5x8"     },
    { u8g2_font_spleen6x12_mr,               "Spleen 6x12"    },
    { u8g2_font_spleen8x16_mr,               "Spleen 8x16"    },
    { u8g2_font_spleen12x24_mr,              "Spleen 12x24"   },
    { u8g2_font_spleen16x32_mr,              "Spleen 16x32"   },
    { u8g2_font_spleen32x64_mr,              "Spleen 32x64"   },
    { u8g2_font_timR08_tr,                   "Times 08"       },
    { u8g2_font_timR10_tr,                   "Times 10"       },
    { u8g2_font_timR12_tr,                   "Times 12"       },
    { u8g2_font_timR14_tr,                   "Times 14"       },
    { u8g2_font_timR18_tr,                   "Times 18"       },
    { u8g2_font_timR24_tr,                   "Times 24"       },
};
const int kReplayFontCount = sizeof(kReplayFonts) / sizeof(kReplayFonts[0]);

const char* fontDisplayName(uint8_t /*family*/, uint8_t size) {
    if (size >= kSizeCount) return "?";
    return kSizeLabels[size];
}
