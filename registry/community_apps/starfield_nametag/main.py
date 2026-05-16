"""Starfield app entry point.

Copyright (c) 2026 Alexandre Roman.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import sys

APP_DIR = "/apps/starfield_nametag"

if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

from badge_app import run_app
import sn_engine as engine
from sn_engine import main, cleanup

TEXT = "Hello, World!"
CONFIG_PATH = "/starfield_nametag_config.json"
BADGE_INFO_PATH = "/badgeInfo.json"


def _load_text():
    import json
    try:
        with open(CONFIG_PATH, "r") as f:
            data = json.load(f)
        candidate = data.get("text")
        if isinstance(candidate, str) and candidate:
            return candidate
    except Exception:
        pass
    try:
        with open(BADGE_INFO_PATH, "r") as f:
            info = json.load(f)
        name = info.get("name")
        if isinstance(name, str):
            first = name.strip().split()
            if first:
                return "Hi, I'm " + first[0] + "!"
    except Exception:
        pass
    return TEXT


engine.set_text(_load_text())

run_app("Starfield", main, cleanup)
