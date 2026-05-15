#!/usr/bin/env python3
"""
Generate StartupFilesData.h + manifests from firmware/initial_filesystem/
plus installable community apps from registry/community_apps/

`firmware/initial_filesystem/` is the **canonical source** of every
file the badge ships with. It's hand-edited and tracked in git.

`firmware/data/` is the **build mirror** PlatformIO's uploadfs reads
from — this script keeps it byte-identical to initial_filesystem/.
data/ is gitignored; never edit it by hand.

Outputs from a single walk:

1) firmware/src/micropython/StartupFilesData.h
   The "survival floor" — only files under BAKE_DIRS ({'lib','matrixApps'})
   are embedded as C string literals into app0. Everything else lives
   on FATFS and is shipped via fatfs.bin / Community Apps / badge_sync.

2) firmware/data/  (mirror of firmware/initial_filesystem/)
   PlatformIO's `pio run -t buildfs` / `uploadfs` consumes this.

3) firmware/initial_filesystem/manifest.json
   Full filesystem manifest (every file, with sha256 + fnv1a32 + size +
   raw URL). Consumed by:
     - badge_sync.py (USB raw-REPL diff sync)
     - JumperIDE "Sync Filesystem" button

4) registry/community_apps.json  (at repo root, NOT under firmware/)
   The Community Apps registry consumed by the on-badge screen.
   Each kind:"app" entry has its full file list inlined under "files":
   [...] — no separate per-app manifest.json needed.
   It includes optional community-only apps from registry/community_apps/.

Usage:
  - Automatic: listed in platformio.ini as
      extra_scripts = pre:scripts/generate_startup_files.py
  - Manual:    python3 scripts/generate_startup_files.py

The script is idempotent: if every output file already has identical
content it skips the write so downstream `make` / SCons don't see a
spurious change.
"""

import hashlib
import json
import os
import sys
from pathlib import Path


# ── Configuration ────────────────────────────────────────────────────────────

# Only files inside these top-level directories are baked into app0. The
# rest of firmware/data/ is shipped via fatfs.bin (factory flash) or
# Community Apps / JumperIDE sync.
BAKE_DIRS = {'lib', 'matrixApps'}

# Dev-only: additional path prefixes that should ALSO be baked into
# kStartupFiles[] for the duration of an iteration cycle. Driven by
# whatever is currently active in BADGE_DEV_FORCE_REFRESH (in
# platformio.ini) — both knobs read from one source of truth so the
# generator and the firmware always agree on which files are
# refreshable.
#
# When this list is empty (production builds) the generator's behavior
# is byte-for-byte identical to before.
def read_dev_bake_paths(project_dir: Path) -> list[str]:
    """Parse `firmware/platformio.ini` for the active (uncommented)
    `-DBADGE_DEV_FORCE_REFRESH="..."` token and return its comma-
    separated path entries.

    Tolerates both `'-DBADGE_DEV_FORCE_REFRESH="..."'` (single-quoted,
    SCons style) and `-DBADGE_DEV_FORCE_REFRESH=...` (no outer quotes).
    Lines starting with `;` are skipped (PlatformIO INI comment).
    """
    pio_ini = project_dir / 'platformio.ini'
    if not pio_ini.exists():
        return []
    paths: list[str] = []
    try:
        with open(pio_ini, 'r', encoding='utf-8') as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith(';') or line.startswith('#'):
                    continue
                if 'BADGE_DEV_FORCE_REFRESH' not in line:
                    continue
                # Extract the substring after the first `=`. The value
                # should be a quoted string for SCons; handle both " and '.
                idx = line.find('=')
                if idx < 0:
                    continue
                tail = line[idx + 1:].strip()
                # Strip any outer single quote (SCons-style wrap).
                if tail.startswith("'") and tail.endswith("'"):
                    tail = tail[1:-1].strip()
                # Strip the inner double quotes.
                if tail.startswith('"'):
                    end = tail.find('"', 1)
                    if end > 1:
                        tail = tail[1:end]
                # Now tail is the comma-separated path list.
                for entry in tail.split(','):
                    entry = entry.strip()
                    # Strip leading slash so we can match against
                    # `rel_path` parts. Trailing `*` or `/` mean prefix
                    # — preserve `/` as the explicit prefix marker.
                    if entry.endswith('*'):
                        entry = entry[:-1]
                    if entry.startswith('/'):
                        entry = entry[1:]
                    if entry:
                        paths.append(entry)
                # First active line wins; ignore further occurrences.
                break
    except OSError:
        return []
    return paths


def _matches_dev_bake(rel_path: str, dev_bake_paths: list[str]) -> bool:
    """Return True if rel_path (no leading `/`) is covered by any of the
    dev-bake prefixes. A prefix ending in `/` requires path to start
    with the prefix; otherwise an exact match is required."""
    for prefix in dev_bake_paths:
        if prefix.endswith('/'):
            if rel_path.startswith(prefix):
                return True
        elif rel_path == prefix:
            return True
    return False

# Top-level dirs whose loose files become kind:"file" registry entries.
# Subdirs of /apps/ become kind:"app" registry entries (one per folder).
LOOSE_FILE_DIRS = {'docs', 'images', 'messages'}

# Files at the data/ root that should also become kind:"file" entries.
LOOSE_ROOT_FILES = {'API_REFERENCE.md', 'messages.json'}

# Skip lists (build artefacts, editor noise).
BINARY_EXTENSIONS = {
    '.bin', '.bmp', '.png', '.jpg', '.jpeg', '.gif', '.ico',
    '.raw', '.pbm', '.xbm', '.fb',
}
PROTECTED_FILENAMES = set()
SKIP_DIR_NAMES = {'__pycache__', 'tests', 'registry'}
SKIP_EXTENSIONS = {'.pyc', '.pyo', '.wad'}
# tests/ is dev-only (uploadfs picks it up but we don't bake or distribute it).
# registry/ is generator output; never recurse into it.

# Default raw-file URL pattern. Override with TEMPORAL_BADGE_RAW_BASE env var
# if you fork the repo. Points at firmware/initial_filesystem/ since that's
# the canonical committed source — firmware/data/ is gitignored.
DEFAULT_RAW_BASE = (
    "https://raw.githubusercontent.com/"
    "temporal-community/badge.temporal.io/main/firmware/initial_filesystem"
)

DEFAULT_COMMUNITY_RAW_BASE = (
    "https://raw.githubusercontent.com/"
    "temporal-community/badge.temporal.io/main/registry/community_apps"
)


# ── FNV-1a (must match the C++ implementation) ──────────────────────────────

def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


# ── Hash history persistence ────────────────────────────────────────────────

def load_hash_history(path: Path) -> dict:
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_hash_history(path: Path, history: dict) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(dict(sorted(history.items())), f, indent=2)
        f.write("\n")


def update_history(history: dict, key: str, current_hash: int) -> list:
    current_hex = f"0x{current_hash:08X}"
    existing = history.get(key, [])
    deduped = [h for h in existing if h.lower() != current_hex.lower()]
    updated = [current_hex] + deduped
    history[key] = updated
    return updated


# ── C code generation helpers ────────────────────────────────────────────────

def is_binary(path: Path) -> bool:
    return path.suffix.lower() in BINARY_EXTENSIONS


def text_to_c_raw_string(content: str) -> str:
    if ')"' in content:
        delimiter = "==="
        attempt = 0
        while f'){delimiter}"' in content and attempt < 10:
            delimiter += "="
            attempt += 1
        return f'R"{delimiter}({content}){delimiter}"'
    return f'R"({content})"'


def bytes_to_c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
    return ",\n".join(lines)


def rel_path_to_var(rel_path: str) -> str:
    name = rel_path.lstrip('/').replace('/', '_').replace('.', '_').replace('-', '_')
    return f"STARTUP_{name.upper()}"


# ── Timestamp-based skip ────────────────────────────────────────────────────

def needs_regeneration(data_dir: Path, output_file: Path,
                       script_file: Path = None) -> bool:
    if not output_file.exists():
        return True
    out_mtime = output_file.stat().st_mtime
    if script_file is not None and script_file.exists() and script_file.stat().st_mtime > out_mtime:
        return True
    for p in data_dir.rglob('*'):
        if any(part in SKIP_DIR_NAMES for part in p.relative_to(data_dir).parts):
            continue
        if p.suffix.lower() in SKIP_EXTENSIONS:
            continue
        if p.stat().st_mtime > out_mtime:
            return True
    return False


def write_if_changed(path: Path, content: str | bytes) -> bool:
    """Write `content` only if the file is missing or differs."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, str):
        if path.exists() and path.read_text(encoding="utf-8") == content:
            return False
        path.write_text(content, encoding="utf-8")
        return True
    if path.exists() and path.read_bytes() == content:
        return False
    path.write_bytes(content)
    return True


# ── File scan ────────────────────────────────────────────────────────────────

def scan_files(data_dir: Path,
                dev_bake_paths: list[str] | None = None) -> list[dict]:
    """Walk firmware/data/ and return a list of file records.

    Each record has rel_path, bytes, fnv (for the bake hash table),
    sha256 (for the manifest / registry), size, and a `bake` flag
    indicating whether the file goes into StartupFilesData.h.

    `dev_bake_paths` is the optional list of additional dev-only
    prefixes pulled from BADGE_DEV_FORCE_REFRESH; any matching file
    is force-baked even if it lives outside BAKE_DIRS."""
    if dev_bake_paths is None:
        dev_bake_paths = []

    files = []
    for path in sorted(data_dir.rglob('*')):
        rel_parts = path.relative_to(data_dir).parts
        if any(part.startswith('.') or part in SKIP_DIR_NAMES for part in rel_parts):
            continue
        if path.suffix.lower() in SKIP_EXTENSIONS:
            continue
        if path.is_dir():
            continue
        rel_path = '/' + path.relative_to(data_dir).as_posix()
        content = path.read_bytes()
        rel_str = path.relative_to(data_dir).as_posix()
        bake = (rel_parts and rel_parts[0] in BAKE_DIRS) or \
               _matches_dev_bake(rel_str, dev_bake_paths)
        files.append({
            'rel_path': rel_path,
            'rel_parts': rel_parts,
            'bytes': content,
            'text': None if is_binary(path) else content.decode('utf-8', errors='replace'),
            'binary': is_binary(path),
            'fnv': fnv1a32(content),
            'sha256': hashlib.sha256(content).hexdigest(),
            'size': len(content),
            'bake': bake,
            'protected': path.name in PROTECTED_FILENAMES,
        })
    return files


def with_raw_base(files: list[dict], raw_base: str) -> list[dict]:
    for f in files:
        f['raw_base'] = raw_base
    return files


# ── Header generation (BAKE set only) ───────────────────────────────────────

def generate_header(bake_files: list[dict], history: dict) -> tuple[str, dict]:
    """Build StartupFilesData.h content + the updated history dict.

    Files outside BAKE_DIRS are *not* listed in kStartupFiles. We still
    record their hashes in startup_hash_history.json so the manifest /
    registry consumer (badge_sync, etc.) has a stable history if it
    needs one — but the badge's boot-time provisioner only sees the
    bake set."""
    L: list[str] = []
    L.append('#pragma once')
    L.append('// ═══════════════════════════════════════════════════════════════════════')
    L.append('// AUTO-GENERATED by scripts/generate_startup_files.py')
    L.append('// DO NOT EDIT — modify files in firmware/data/ and rebuild.')
    L.append('//')
    L.append('// Only files under BAKE_DIRS = {lib, matrixApps} are embedded here.')
    L.append('// Everything else ships via fatfs.bin (factory flash) or Community Apps')
    L.append('// / JumperIDE sync — see firmware/docs/STORAGE-MODEL.md.')
    L.append('// ═══════════════════════════════════════════════════════════════════════')
    L.append('')
    L.append('#include <stdint.h>')
    L.append('')
    L.append('#define STARTUP_FILE_PROTECTED  (1 << 0)')
    L.append('')
    L.append('struct StartupFileInfo {')
    L.append('    const char* path;')
    L.append('    const char* content;')
    L.append('    uint32_t contentLen;')
    L.append('    const uint32_t* knownHashes;')
    L.append('    int hashCount;')
    L.append('    uint8_t flags;')
    L.append('};')
    L.append('')

    L.append('// ─── Embedded file contents ──────────────────────────────────────────')
    L.append('')

    var_for: dict[str, str] = {}
    for f in bake_files:
        v = rel_path_to_var(f['rel_path'])
        var_for[f['rel_path']] = v
        all_hashes = update_history(history, f['rel_path'], f['fnv'])
        f['all_hashes'] = all_hashes
        if f['binary']:
            L.append(f'static const uint8_t {v}_DATA[] = {{')
            L.append(bytes_to_c_array(f['bytes']))
            L.append('};')
            L.append(f'static const uint32_t {v}_LEN = sizeof({v}_DATA);')
        else:
            L.append(f'static const char {v}_DATA[] = {text_to_c_raw_string(f["text"])};')
        hashes_str = ', '.join(all_hashes)
        L.append(f'static const uint32_t {v}_HASHES[{len(all_hashes)}] = {{ {hashes_str} }};')
        L.append('')

    L.append('// ─── File table ───────────────────────────────────────────────────────')
    L.append('')
    L.append('static const StartupFileInfo kStartupFiles[] = {')
    for f in bake_files:
        v = var_for[f['rel_path']]
        flags = 'STARTUP_FILE_PROTECTED' if f['protected'] else '0'
        if f['binary']:
            content_expr = f'(const char*){v}_DATA'
            size_expr = f'{v}_LEN'
        else:
            content_expr = f'{v}_DATA'
            size_expr = f'sizeof({v}_DATA) - 1'
        count = len(f['all_hashes'])
        L.append(f'    {{ "{f["rel_path"]}", {content_expr}, {size_expr}, {v}_HASHES, {count}, {flags} }},')
    if not bake_files:
        L.append('    { nullptr, nullptr, 0, nullptr, 0, 0 }')
    L.append('};')
    L.append(f'static const int kStartupFileCount = {len(bake_files)};')
    L.append('')

    # Managed directories — every directory under BAKE_DIRS that we
    # actually emitted a file for, plus every ancestor up to the root.
    # provisionStartupFiles iterates this list calling f_mkdir; FATFS
    # f_mkdir won't auto-create missing ancestors, so we have to list
    # them explicitly. Sorted so parents appear before children.
    bake_dirs_set = set()
    for f in bake_files:
        if len(f['rel_parts']) <= 1:
            continue
        parent_parts = f['rel_parts'][:-1]
        for i in range(1, len(parent_parts) + 1):
            bake_dirs_set.add('/' + '/'.join(parent_parts[:i]))
    bake_dirs = sorted(bake_dirs_set)
    L.append('// ─── Managed directories ─────────────────────────────────────────────')
    L.append('')
    if bake_dirs:
        L.append('static const char* kStartupDirs[] = {')
        for d in bake_dirs:
            L.append(f'    "{d}",')
        L.append('};')
    else:
        L.append('static const char* kStartupDirs[] = { nullptr };')
    L.append(f'static const int kStartupDirCount = {len(bake_dirs)};')
    L.append('')

    return '\n'.join(L).rstrip() + '\n', history


# ── Manifest generation (FULL set) ──────────────────────────────────────────

def generate_full_manifest(files: list[dict], raw_base: str) -> str:
    """Emit firmware/data/manifest.json — every file in firmware/data/.

    Used by badge_sync (the diff engine) and by JumperIDE."""
    entries = []
    for f in files:
        entries.append({
            "path": f['rel_path'],
            "size": f['size'],
            "sha256": f['sha256'],
            "fnv1a32": f"0x{f['fnv']:08X}",
            "url": f"{raw_base}{f['rel_path']}",
            "baked": f['bake'],
        })
    return json.dumps({
        "schema_version": 2,
        "generator": "generate_startup_files.py",
        "raw_base": raw_base,
        "files": entries,
    }, indent=2) + "\n"


def generate_community_apps(files: list[dict], data_dir: Path,
                            raw_base: str,
                            curated_registry: Path = None) -> str:
    """Emit registry/community_apps.json.

    Sources:
      - <name>/  → kind:"app", dest_dir=/apps/<name>, files: [...]
      - <file>   → kind:"file" (loose top-level scripts like hello.py)
      - docs/, images/, messages/  → kind:"file"
      - data/ root files in LOOSE_ROOT_FILES → kind:"file"
      - curated_registry (registry/registry.json schema v1) → kind:"file"
        Used for big assets that don't ship in initial_filesystem/
        and instead live as GitHub release attachments — doom1.wad
        is the canonical example. This lets the badge see the full
        asset catalog from one HTTP fetch.

    For app entries the per-file list (path/sha256/size/url) is
    inlined directly rather than referenced through a per-app
    manifest.json — keeps the registry fully self-describing in one
    HTTP fetch and avoids cluttering app folders.
    """
    apps: dict[str, list[dict]] = {}
    app_metadata: dict[str, dict] = {}
    file_entries: list[dict] = []
    for f in files:
        parts = f['rel_parts']
        if not parts:
            continue
        head = parts[0]
        if len(parts) >= 2:
            if parts[-1] == 'community.json' and not f['binary']:
                try:
                    app_metadata[parts[0]] = json.loads(f['text'])
                except Exception as exc:
                    print(
                        f"[generate_startup_files] WARNING: could not parse "
                        f"{f['rel_path']}: {exc}"
                    )
                continue
            # <name>/<file...> — bundle entry.
            apps.setdefault(parts[0], []).append(f)
        elif len(parts) == 1:
            # Top-level files in registry/community_apps are docs or
            # maintainer notes, not badge-installable files.
            if parts[0].lower() == 'readme.md':
                continue
            file_entries.append(f)
        elif head in LOOSE_FILE_DIRS or (
                len(parts) == 1 and parts[0] in LOOSE_ROOT_FILES):
            file_entries.append(f)
        # Skip BAKE_DIRS; they're not user-installable.

    assets = []

    # App entries.
    for name in sorted(apps.keys()):
        app_files = sorted(apps[name], key=lambda f: f['rel_path'])
        meta = app_metadata.get(name, {})
        size = sum(f['size'] for f in app_files)
        # Stable version derived from sha256(concat of per-file sha256s)
        # so any change to any file in the bundle bumps the version.
        h = hashlib.sha256()
        for f in app_files:
            h.update(f['sha256'].encode())
        version = h.hexdigest()[:12]
        # Try to lift a description from the app's main.py docstring;
        # fall back to the folder name.
        display_name = meta.get('name') or name.replace('_', ' ').title()
        desc = meta.get('description') or display_name
        for f in app_files:
            if meta.get('description'):
                break
            if f['rel_parts'][-1] == 'main.py' and not f['binary']:
                lines = f['text'].splitlines()
                if lines and lines[0].strip().startswith('"""'):
                    body = []
                    for line in lines[1:]:
                        if '"""' in line:
                            break
                        body.append(line.strip())
                    if body:
                        desc = ' '.join(body[:3])[:480] or desc
                break
        # Inlined per-file list — paths are *relative to dest_dir* so
        # the on-badge installer can write them with `dest_dir + path`.
        bundle = []
        for f in app_files:
            rel = '/' + '/'.join(f['rel_parts'][1:])  # strip <name>/
            url_base = f.get('raw_base', raw_base)
            bundle.append({
                "path": rel,
                "size": f['size'],
                "sha256": f['sha256'],
                "url": f"{url_base}{f['rel_path']}",
            })
        entry = {
            "id": name.replace('_', '-')[:32],
            "kind": "app",
            "name": display_name,
            "version": version,
            "dest_dir": f"/apps/{name}",
            "size": size,
            "min_free_bytes": size + 4096,
            "description": desc,
            "files": bundle,
        }
        contributor_names = meta.get('contributors') or meta.get('community_contributors') or []
        if isinstance(contributor_names, str):
            contributor_names = [contributor_names]
        if contributor_names:
            entry["community_contributors"] = contributor_names
        if any(f.get('community_contributor') for f in app_files):
            contributors = sorted({
                f['community_contributor']
                for f in app_files
                if f.get('community_contributor')
            })
            entry["community_contributors"] = contributors
        assets.append(entry)

    # File entries.
    for f in sorted(file_entries, key=lambda f: f['rel_path']):
        stem_id = f['rel_path'].lstrip('/').replace('/', '-').replace('.', '-')[:32]
        url_base = f.get('raw_base', raw_base)
        assets.append({
            "id": stem_id,
            "kind": "file",
            "name": f['rel_path'].split('/')[-1],
            "version": f['sha256'][:12],
            "dest_path": f['rel_path'],
            "url": f"{url_base}{f['rel_path']}",
            "sha256": f['sha256'],
            "size": f['size'],
            "min_free_bytes": f['size'] + 1024,
            "description": f"Optional asset ({f['rel_path']})",
        })

    # Curated big-file assets from registry/registry.json. These are
    # files too large to live in initial_filesystem/ (gitignored,
    # shipped as GitHub release attachments). doom1.wad is the
    # canonical example — ~4 MB WAD installed via the
    # /releases/latest/download/<name> URL pattern. We dedup on
    # dest_path so a hand-curated entry can't shadow a generated one.
    existing_dests = {a.get("dest_path") for a in assets if "dest_path" in a}
    if curated_registry and curated_registry.exists():
        try:
            curated = json.loads(curated_registry.read_text())
        except json.JSONDecodeError as exc:
            print(f"[generate_startup_files] WARNING: {curated_registry} "
                  f"unparseable: {exc}; skipping curated merge")
            curated = None
        if curated:
            for entry in curated.get("assets", []) or []:
                dp = entry.get("dest_path")
                if not dp:
                    continue  # curated apps would need kind:"app" + files[]
                if dp in existing_dests:
                    continue
                # registry.json schema v1 lacks the explicit `kind`
                # field; everything in there is a single file today.
                merged = {
                    "id": entry.get("id", dp.lstrip("/").replace("/", "-")[:32]),
                    "kind": "file",
                    "name": entry.get("name", dp.rsplit("/", 1)[-1]),
                    "version": entry.get("version", "1"),
                    "dest_path": dp,
                    "url": entry["url"],
                    "size": entry.get("size", 0),
                    "min_free_bytes": entry.get("min_free_bytes",
                                                int(entry.get("size", 0)) + 4096),
                    "description": entry.get("description", ""),
                }
                if entry.get("sha256"):
                    merged["sha256"] = entry["sha256"]
                assets.append(merged)
                existing_dests.add(dp)

    return json.dumps({
        "schema_version": 2,
        "generator": "generate_startup_files.py",
        "assets": assets,
    }, indent=2) + "\n"


# ── Main generation ─────────────────────────────────────────────────────────

def generate(project_dir: Path, script_file: Path = None):
    src_dir = project_dir / 'initial_filesystem'  # canonical
    mirror_dir = project_dir / 'data'             # uploadfs build mirror
    output_header = project_dir / 'src' / 'micropython' / 'StartupFilesData.h'
    history_file = project_dir / 'scripts' / 'startup_hash_history.json'
    full_manifest_file = src_dir / 'manifest.json'
    # registry/ lives at the repo root so it has a stable raw URL the
    # badge can hit (firmware/initial_filesystem/ being part of the
    # raw URL would also work, but keeping the registry one click
    # off the repo root is friendlier for forks / external CDNs).
    repo_root = project_dir.parent if project_dir.name == 'firmware' else project_dir
    community_src_dir = repo_root / 'registry' / 'community_apps'
    registry_file = repo_root / 'registry' / 'community_apps.json'
    # Hand-curated big-file assets (release attachments like doom1.wad).
    # Merged into community_apps.json so the badge sees the full
    # catalog from one fetch.
    curated_registry_file = repo_root / 'registry' / 'registry.json'

    if not src_dir.exists():
        print(f"[generate_startup_files] WARNING: {src_dir} not found, generating empty header")
        write_if_changed(output_header,
            '#pragma once\n'
            '#include <stdint.h>\n\n'
            'struct StartupFileInfo { const char* path; const char* content;\n'
            '    uint32_t contentLen; const uint32_t* knownHashes;\n'
            '    int hashCount; uint8_t flags; };\n\n'
            'static const StartupFileInfo kStartupFiles[] = { {nullptr,nullptr,0,nullptr,0,0} };\n'
            'static const int kStartupFileCount = 0;\n'
            'static const char* kStartupDirs[] = { nullptr };\n'
            'static const int kStartupDirCount = 0;\n')
        return

    needs_run = needs_regeneration(src_dir, output_header, script_file)
    needs_mirror = not mirror_dir.exists() or needs_run

    if not needs_run:
        if full_manifest_file.exists() and registry_file.exists() and not needs_mirror:
            print("[generate_startup_files] Up to date, skipping")
            return

    print("[generate_startup_files] Scanning firmware/initial_filesystem/ ...")

    history = load_hash_history(history_file)

    # Dev-bake — pull any active BADGE_DEV_FORCE_REFRESH paths from
    # platformio.ini and add them to the bake set so the firmware ships
    # them in app0 (and shouldForceRefresh() at boot can overwrite the
    # stale on-FAT versions). Empty list = production mode = no-op.
    dev_bake_paths = read_dev_bake_paths(project_dir)
    if dev_bake_paths:
        print("[generate_startup_files] DEV BAKE active "
              "(paths from platformio.ini BADGE_DEV_FORCE_REFRESH):")
        for p in dev_bake_paths:
            print(f"    {p}")
        print("[generate_startup_files] DEV BAKE: extra files will be "
              "embedded in firmware app0 — REMOVE the build flag for "
              "production builds")

    files = scan_files(src_dir, dev_bake_paths=dev_bake_paths)

    bake_files = [f for f in files if f['bake']]
    raw_base = os.environ.get("TEMPORAL_BADGE_RAW_BASE", DEFAULT_RAW_BASE)
    community_raw_base = os.environ.get(
        "TEMPORAL_BADGE_COMMUNITY_RAW_BASE",
        DEFAULT_COMMUNITY_RAW_BASE,
    )

    if dev_bake_paths:
        extra = [f for f in bake_files
                  if f['rel_parts']
                  and f['rel_parts'][0] not in BAKE_DIRS]
        if extra:
            total_bytes = sum(f['size'] for f in extra)
            print(f"[generate_startup_files] DEV BAKE: "
                  f"{len(extra)} extra files, {total_bytes} bytes")

    # 1) StartupFilesData.h (bake set only).
    header_text, history = generate_header(bake_files, history)
    save_hash_history(history_file,
        {k: v for k, v in history.items()
         if any(f['rel_path'] == k for f in bake_files)})
    if write_if_changed(output_header, header_text):
        print(f"[generate_startup_files] wrote {output_header.name} "
              f"({len(bake_files)} baked files)")
    else:
        print(f"[generate_startup_files] {output_header.name} unchanged")

    # 2) firmware/data/manifest.json (every file).
    manifest_text = generate_full_manifest(files, raw_base)
    if write_if_changed(full_manifest_file, manifest_text):
        print(f"[generate_startup_files] wrote {full_manifest_file.relative_to(project_dir)} "
              f"({len(files)} files)")

    # Clean up any stale per-app manifest.json files left over from
    # the older schema where each app folder had its own manifest.
    # The same data is now inlined into community_apps.json.
    apps_root = src_dir / 'apps'
    if apps_root.exists():
        for app_dir in apps_root.iterdir():
            stale = app_dir / 'manifest.json'
            if stale.is_file():
                stale.unlink()

    # 3) registry/community_apps.json (with inlined per-app file lists,
    #    plus any curated big-file assets merged from registry.json).
    #
    # The factory filesystem already includes firmware/initial_filesystem/.
    # Keep the public Community Apps catalog focused on optional remote
    # installs so the badge does not list baked-in apps as downloads.
    registry_files = []
    if community_src_dir.exists():
        community_files = scan_files(community_src_dir)
        for f in community_files:
            f['bake'] = False
        registry_files.extend(with_raw_base(community_files,
                                            community_raw_base))

    registry_text = generate_community_apps(registry_files, src_dir, raw_base,
                                            curated_registry_file)
    if write_if_changed(registry_file, registry_text):
        try:
            display = registry_file.relative_to(project_dir)
        except ValueError:
            display = registry_file
        print(f"[generate_startup_files] wrote {display}")

    # 5) Mirror initial_filesystem/ → data/ for `pio run -t buildfs/uploadfs`.
    #    Done last so the per-app manifest.json files we just wrote also
    #    propagate. We mirror at file granularity (not a blanket rmtree+cp)
    #    so unchanged files keep their mtimes, which avoids spurious
    #    fatfs.bin rebuilds on every PlatformIO run.
    mirror_files(src_dir, mirror_dir)


def mirror_files(src: Path, dst: Path) -> None:
    """Byte-mirror `src` → `dst`, preserving mtimes on unchanged files.

    Mirror policy is broader than the manifest's: we keep `tests/` in
    the mirror (dev builds want them via `pio run -t uploadfs`) even
    though they're filtered out of the registry/manifest. We do skip
    editor noise and Python build artefacts."""
    import shutil

    MIRROR_SKIP_PARTS = {'__pycache__', '.DS_Store'}
    MIRROR_SKIP_SUFFIX = {'.pyc', '.pyo'}

    def skip(parts) -> bool:
        return any(p.startswith('.') or p in MIRROR_SKIP_PARTS for p in parts)

    seen: set[Path] = set()
    for p in src.rglob('*'):
        rel = p.relative_to(src)
        if skip(rel.parts):
            continue
        if p.suffix in MIRROR_SKIP_SUFFIX:
            continue
        target = dst / rel
        if p.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            seen.add(target)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if (not target.exists() or
                target.stat().st_size != p.stat().st_size or
                target.read_bytes() != p.read_bytes()):
            shutil.copy2(p, target)
        seen.add(target)

    # Prune anything in dst that no longer has a counterpart in src.
    if dst.exists():
        for p in sorted(dst.rglob('*'), reverse=True):
            if p in seen:
                continue
            if skip(p.relative_to(dst).parts):
                continue
            try:
                if p.is_dir():
                    p.rmdir()
                else:
                    p.unlink()
            except OSError:
                pass


# ── Entry points ─────────────────────────────────────────────────────────────

try:
    Import("env")  # type: ignore  # PlatformIO SCons global
    _proj = Path(env.subst("$PROJECT_DIR"))  # type: ignore
    _script = _proj / "scripts" / "generate_startup_files.py"
    generate(_proj, _script if _script.exists() else None)
except NameError:
    # Running standalone (python3 scripts/generate_startup_files.py)
    _sf = Path(__file__)
    generate(_sf.parent.parent, _sf)
