"""Flappy Asteroids app entry point."""

import sys

APP_DIR = "/apps/flappy_asteroids"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
from fa_engine import main

run_app("Flappy Asteroids", main)
