"""IR Playground \u2014 home grid.

Four-tile launcher that visually mirrors the badge's home GridMenuScreen
but lives in MicroPython. Each tile owns a separate consolidated app:

  REMOTE  \u2014 TV / Audio / Projector / AC / Custom / Macro / TV-B-Gone
  SCAN    \u2014 Live / Detail / Scope IR signal inspection
  RANGE   \u2014 Bounce / Sweep / MinPwr signal-strength diagnostics
  GAMES   \u2014 Echo / QuickDraw / Beacon / Disco peer mini-games
"""

# AppRegistry dunders — these are scanned at boot to populate the home
# grid for auto-discovered Python apps. The curated C++ tile in
# firmware/src/ui/GUI.cpp uses the label "IR PLAY"; the dedupe in
# rebuildMainMenuFromRegistry() suppresses the auto-discovered tile when
# this title matches that label, so the user sees one icon instead of
# two ("IR PLAY" curated + "Ir Remote" auto).
__title__ = "IR PLAY"
__description__ = "Universal remote, sniffer, TV-B-Gone, IR mini-games"
__icon__ = "icon.py"

import sys

APP_DIR = "/apps/ir_remote"
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge import *
from badge_app import run_app

import ir_lib as L
import remote as REMOTE
import scan as SCAN
import range as RANGE
import games as GAMES


def main():
    items = (
        {"label": "REMOTE", "icon": "ir_play",  "desc": "Control TVs, ACs, audio gear",   "fn": REMOTE.run},
        {"label": "SCAN",   "icon": "booper",   "desc": "Sniff and decode IR signals",    "fn": SCAN.run},
        {"label": "RANGE",  "icon": "wifi",     "desc": "Test signal range with peers",   "fn": RANGE.run},
        {"label": "GAMES",  "icon": "games",    "desc": "IR mini-games for two badges",   "fn": GAMES.run},
    )
    while True:
        idx = L.grid_menu(list(items), title="IR Playground")
        if idx < 0:
            return
        items[idx]["fn"]()


run_app("IR Playground", main)
