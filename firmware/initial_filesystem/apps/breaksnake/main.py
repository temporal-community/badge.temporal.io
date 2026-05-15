"""BreakSnake app entry point."""

import sys

APP_DIR = "/apps/breaksnake"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
from bs_engine import main

run_app("BreakSnake", main)
