#pragma once

// Provision files from the embedded initial_filesystem/ snapshot.
//
// Normal mode (forceSync=false):
//   - Creates any missing files
//   - Overwrites files that match a known older firmware hash (unmodified defaults)
//   - Leaves user-modified files untouched
//   - Honors per-file dev force-refresh overrides — see below
//
// Force-sync mode (forceSync=true):
//   - Overwrites every file to match initial_filesystem/
//   - Removes files from managed directories that aren't in initial_filesystem/
//
// Call once after the FAT filesystem is mounted (after mpy_start).
//
// ── Dev force-refresh ──────────────────────────────────────────────────────
//
// When iterating on a Python app it's painful to discover the on-FAT
// version is being preserved as "user-modified" because the developer
// edited it in JumperIDE between build cycles. Two opt-in ways to force
// specific files (or prefixes) to overwrite-on-boot:
//
//   1. Compile-time: pass `-DBADGE_DEV_FORCE_REFRESH=PATHS` where PATHS
//      is a comma-separated string of full paths or prefixes.
//      Examples (in platformio.ini under [env:echo-dev] for instance):
//          build_flags = -DBADGE_DEV_FORCE_REFRESH=\"/apps/ir_remote/\"
//          build_flags = -DBADGE_DEV_FORCE_REFRESH=\"/apps/foo/,/lib/badge_ui.py\"
//      Entries ending in `/` or `*` are matched as prefixes; everything
//      else is an exact path match. Active for every boot of that
//      firmware build. Safe for production builds — leave the flag
//      undefined and the helper compiles to a no-op.
//
//   2. Runtime marker: drop a `/dev_force_refresh.txt` file on the badge
//      (via JumperIDE, `pio run -t uploadfs`, or the badge's serial
//      console) containing one path/prefix per line. Comments after
//      `#` are ignored. The marker is consumed (deleted) after the
//      first provisioning pass so the force is a one-shot. Useful when
//      you don't want to rebuild firmware just to refresh one file.
//
// Both knobs feed into the same per-file matcher inside provisionStartupFiles.

void provisionStartupFiles(bool forceSync = false);

// Reformat the ffat partition in place (`f_mkfs(FM_ANY | FM_SFD)`)
// and immediately reprovision the embedded startup files. Used when
// the on-device FAT is wedged (e.g. reports 0 free bytes despite the
// partition being mostly empty — usually a stale FAT structure left
// behind by an older partitions.csv layout). Loses all on-FAT files
// (settings.txt, /apps/<slug>/save.json, /tardigrade_save.json, etc.);
// NVS state is untouched. Returns true on success.
bool formatAndReprovisionFFat();
