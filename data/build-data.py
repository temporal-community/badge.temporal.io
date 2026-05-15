#!/usr/bin/env python3
"""
build-data.py — build data/out/*.{json,msgpack.txt} from data/in/.

Sources of truth:
    data/in/speakers.md     markdown table
    data/in/floors.md       markdown sections
    data/in/schedule.yaml   YAML

Outputs (consumed by ESP32 firmware via deserializeMsgPack):
    data/out/speakers.{json,msgpack.txt}
    data/out/floors.{json,msgpack.txt}
    data/out/schedule.{json,msgpack.txt}

The .msgpack.txt files contain raw MessagePack bytes — serve them with
Content-Type: application/msgpack and call deserializeMsgPack() in firmware.

Loc-code scheme
    section:     floor_idx * 100 + section_idx * 10
    whole floor: floor_idx * 100 + 99       (e.g. "Lobby" = 199)
    off-site:    900 + section_idx * 10     (override, per spec: prefix 9)
    no location: field omitted

Usage:
    pip install msgpack pyyaml
    python3 build-data.py
"""

import json
import re
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import msgpack
    import yaml
except ImportError:
    sys.exit("ERROR: pip install msgpack pyyaml")

HERE = Path(__file__).resolve().parent
IN   = HERE / "in"
OUT  = HERE / "out"
OUT.mkdir(exist_ok=True)

TYPE_MAP     = {"talk": 0, "workshop": 1, "other": 2}
OFFSITE_BASE = 900   # spec: off-site events use 9-prefix loc codes

# bundle header: magic, version, flags, crc32, total_size, sched_len, speak_len, floors_len
BUNDLE_MAGIC   = 0x53444254  # 'TBDS' little-endian
BUNDLE_VERSION = 1
BUNDLE_HEADER_FMT = "<IHHIIIII"     # 28 bytes
BUNDLE_HEADER_SZ  = struct.calcsize(BUNDLE_HEADER_FMT)


# ──────────────────────────── speakers.md ────────────────────────────

def parse_speakers(md):
    rows = []
    for line in md.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 4 or cells[0] == "Name" or cells[0].startswith("---"):
            continue
        rows.append({
            "id":    len(rows),
            "name":  cells[0],
            "co":    cells[1],
            "title": cells[2],
            "bio":   cells[3],
        })
    return rows


# ──────────────────────────── floors.md ──────────────────────────────

_SECTION_RE = re.compile(
    r"^\d+\.\s+\*\*(?P<title>[^*]+)\*\*"
    r"(?:\s*\((?P<alias>[^)]+)\))?"
    r"(?:\s*`icon:\s*(?P<icon>[a-z_-]+)`)?"
    r"(?:\s*`[^`]+`)*"                       # tolerate any extra inline-code tokens
                                              # (e.g. `sponsors: foo, bar`) after the icon
    r"(?:\s*[—\-]+\s*(?P<desc>.+))?$"
)

def parse_floors(md):
    floors = []
    cur = None
    last = None  # last section, awaiting a possible next-line description

    for raw in md.splitlines():
        line = raw.strip()

        if line.startswith("# "):
            cur = {
                "idx":      len(floors),
                "title":    line[2:].strip(),
                "subtitle": "",
                "alias":    None,
                "sections": [],
            }
            floors.append(cur)
            last = None
            continue

        if not cur:
            continue

        if not cur["subtitle"] and line.startswith("*") and line.endswith("*"):
            cur["subtitle"] = line.strip("*").strip()
            m = re.match(r"^(\S+)\s+level\b", cur["subtitle"], re.IGNORECASE)
            if m:
                cur["alias"] = m.group(1)
            continue

        m = _SECTION_RE.match(line)
        if m:
            sec = {
                "idx":   len(cur["sections"]),
                "title": m.group("title").strip(),
                "alias": (m.group("alias") or "").strip() or None,
                "icon":  m.group("icon"),
                "desc":  (m.group("desc") or "").strip() or None,
            }
            cur["sections"].append(sec)
            last = sec
            continue

        if last and last["desc"] is None and line and not line.startswith("---"):
            last["desc"] = line
            last = None

    # assign loc codes
    for fl in floors:
        base = OFFSITE_BASE if fl["title"].lower() in ("off-site", "offsite") \
                            else fl["idx"] * 100
        fl["base"] = base
        for s in fl["sections"]:
            s["loc"] = base + s["idx"] * 10

    return floors


def build_loc_lookup(floors):
    """name → loc; whole-floor refs use base+99."""
    lookup = {}
    for fl in floors:
        whole = fl["base"] + 99
        lookup[fl["title"]] = whole
        if fl["alias"]:
            lookup[fl["alias"]] = whole
        for s in fl["sections"]:
            lookup[s["title"]] = s["loc"]
            if s["alias"]:
                lookup[s["alias"]] = s["loc"]
    return lookup


def clean_floors(floors):
    out = []
    for fl in floors:
        f = {"idx": fl["idx"], "title": fl["title"],
             "sections": [], "desc": fl["subtitle"]}
        for s in fl["sections"]:
            sec = {"idx": s["idx"], "loc": s["loc"], "title": s["title"]}
            if s["alias"]: sec["alias"] = s["alias"]
            if s["icon"]:  sec["icon"]  = s["icon"]
            if s["desc"]:  sec["desc"]  = s["desc"]
            f["sections"].append(sec)
        out.append(f)
    return out


# ──────────────────────────── schedule.yaml ──────────────────────────

def parse_time(s):
    h, m = s.split(":")
    return int(h) * 100 + int(m)


def build_schedule(yaml_data, speakers, floors):
    name2id  = {s["name"]: s["id"] for s in speakers}
    loc      = build_loc_lookup(floors)
    missing  = []
    days     = []

    for day in yaml_data:
        events = []
        for ev in day["events"]:
            out = {
                "type":  TYPE_MAP[ev["type"]],
                "title": ev["title"],
                "start": parse_time(ev["start"]),
                "end":   parse_time(ev["end"]),
            }
            if "location" in ev:
                code = loc.get(ev["location"])
                if code is None:
                    missing.append(("location", ev["location"], ev["title"]))
                else:
                    out["loc"] = code
            if "tags" in ev:
                out["tags"] = ",".join(ev["tags"])
            if "desc" in ev:
                out["desc"] = ev["desc"]
            if "speakers" in ev:
                ids = []
                for n in ev["speakers"]:
                    if n in name2id:
                        ids.append(name2id[n])
                    else:
                        missing.append(("speaker", n, ev["title"]))
                out["speakers"] = ids
            events.append(out)
        days.append(events)
    return days, missing


# ──────────────────────────── main ───────────────────────────────────

def main():
    speakers   = parse_speakers((IN / "speakers.md").read_text())
    floors_int = parse_floors((IN / "floors.md").read_text())
    sched_yaml = yaml.safe_load((IN / "schedule.yaml").read_text())
    schedule, missing = build_schedule(sched_yaml, speakers, floors_int)
    floors     = clean_floors(floors_int)

    if missing:
        print("WARNING: unresolved references")
        for kind, name, where in missing:
            print(f"    {kind}: {name!r}  (event: {where!r})")
        print()

    print(f"Writing → {OUT}")
    packs = {}
    for name, data in (("speakers", speakers),
                       ("floors",   floors),
                       ("schedule", schedule)):
        (OUT / f"{name}.json").write_text(
            json.dumps(data, indent=2, ensure_ascii=False))
        packed  = msgpack.packb(data, use_bin_type=True)
        json_sz = len(json.dumps(data, separators=(",", ":")).encode())
        saved   = 100 * (1 - len(packed) / json_sz)
        (OUT / f"{name}.msgpack.txt").write_bytes(packed)
        packs[name] = packed
        print(f"  {name:<9}  json={json_sz:>5} B   msgpack={len(packed):>5} B   ({saved:+.1f}%)")

    # Combined bundle: header + 3 msgpack blobs, CRC32 over [crc_offset+4..end]
    # Layout: magic u32 | version u16 | flags u16 | crc32 u32 | total_sz u32 |
    #         sched_len u32 | speak_len u32 | floors_len u32 | <payloads...>
    payload = packs["schedule"] + packs["speakers"] + packs["floors"]
    total_sz = BUNDLE_HEADER_SZ + len(payload)
    # body = bytes covered by CRC: total_sz + 3 lens + payloads
    body = struct.pack("<IIII",
                       total_sz,
                       len(packs["schedule"]),
                       len(packs["speakers"]),
                       len(packs["floors"])) + payload
    crc = zlib.crc32(body) & 0xffffffff
    header = struct.pack("<IHHI",
                         BUNDLE_MAGIC, BUNDLE_VERSION, 0, crc)
    bundle = header + body
    assert len(bundle) == total_sz
    (OUT / "bundle.bin").write_bytes(bundle)
    print(f"  bundle     total ={len(bundle):>5} B   crc=0x{crc:08x}   ts={int(time.time())}")

    print(f"\n  speakers={len(speakers)}   "
          f"floors={len(floors)}   "
          f"days={len(schedule)}   "
          f"events={sum(len(d) for d in schedule)}")


if __name__ == "__main__":
    main()
