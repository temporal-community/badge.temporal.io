// BadgeVersion.h — Firmware version string
//
// Single source of truth: firmware/VERSION (a one-line semver like "0.1.4").
// scripts/inject_version.py reads it on every PlatformIO build and injects
//   -DFIRMWARE_VERSION="<ver>"        (consumed here)
//   -DBADGE_FIRMWARE_VERSION="<ver>"  (consumed by lib/micropython_embed/
//                                      src/mpconfigport.h for the REPL
//                                      banner "Replay Badge v<ver> with
//                                      ESP32-S3")
//
// To bump the version: edit firmware/VERSION (or run
// firmware/scripts/bump_version.sh <new-version>) and rebuild.
//
// The fallback below only applies when building outside PlatformIO
// (e.g. plain Arduino IDE or a partial pio invocation that skips the
// pre-build script). In that case the firmware still compiles but
// reports "dev".

#pragma once

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "dev"
#endif
