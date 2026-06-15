import sys

APP_DIR = "/apps/tardigotchi"
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
from engine import main

run_app("Tardigotchi", main)
