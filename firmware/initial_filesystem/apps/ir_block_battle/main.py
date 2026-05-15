"""IR Block Battle app entry point."""

import sys

APP_DIR = "/apps/ir_block_battle"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from ibb_engine import main
from badge_app import run_app

run_app("IR Block Battle", main)
