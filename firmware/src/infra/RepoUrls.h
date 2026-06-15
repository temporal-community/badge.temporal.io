// RepoUrls.h — Single source of truth for all GitHub repo-derived URLs.
//
// To migrate to a new GitHub org/repo, change ONLY the REPO_OWNER_SLUG and
// REPO_NAME defines (or override REPO_OWNER_SLUG at compile time via
// `-DREPO_OWNER_SLUG="org/repo"` in platformio.ini). Every derived URL macro
// below recompiles automatically via C string literal concatenation.
//
// Fork support: platformio.ini sets `-DREPO_OWNER_SLUG=...` so CI builds
// targeting a different repo are one line in platformio.ini; no source edits.
//
// Backwards compatibility: OTA_GITHUB_REPO is aliased to REPO_OWNER_SLUG so
// any out-of-tree code that still references OTA_GITHUB_REPO keeps working.

#pragma once

// ── Identity ─────────────────────────────────────────────────────────────────
// To migrate: comment out the active line, uncomment the target line.
// Also update REPO_SLUG / DEFAULT_BRANCH in scripts/repo_urls.py to match.

#ifndef REPO_OWNER_SLUG
// Temporal-hosted repo (active for the public release):
#define REPO_OWNER_SLUG "temporal-community/badge.temporal.io"
// Original repo (kept for reference):
// #define REPO_OWNER_SLUG "Architeuthis-Flux/Temporal-Replay-26-Badge"
#endif

#ifndef REPO_DEFAULT_BRANCH
#define REPO_DEFAULT_BRANCH "main"
#endif

// ── Derived URL macros (do not edit) ─────────────────────────────────────────
// All are fully-formed string literals via compile-time concatenation.

// GitHub Releases REST API — returns JSON with tag_name + assets[].
#define REPO_RELEASES_API_URL \
    "https://api.github.com/repos/" REPO_OWNER_SLUG "/releases/latest"

// GitHub releases/latest redirect — points to .../releases/tag/<tag>.
// Rate-limit-immune fallback for tag discovery.
#define REPO_RELEASES_LATEST_URL \
    "https://github.com/" REPO_OWNER_SLUG "/releases/latest"

// snprintf format for a specific release asset download URL.
// Arguments: tag (const char*), asset name (const char*).
#define REPO_RELEASE_DOWNLOAD_FMT \
    "https://github.com/" REPO_OWNER_SLUG "/releases/download/%s/%s"

// Base URL for raw file access under this repo + branch.
#define REPO_RAW_BASE \
    "https://raw.githubusercontent.com/" REPO_OWNER_SLUG \
    "/" REPO_DEFAULT_BRANCH

// Community Apps registry JSON — fetched once a day by AssetRegistry.
#define REPO_COMMUNITY_APPS_URL \
    REPO_RAW_BASE "/registry/community_apps.json"

// Recovery docs URL embedded in the QR code on the OTA recovery screen.
#define REPO_RECOVERY_URL \
    "https://github.com/" REPO_OWNER_SLUG \
    "/blob/" REPO_DEFAULT_BRANCH "/firmware/docs/OTA-MAINTAINER.md#recovery"

// ── Backwards-compat alias ───────────────────────────────────────────────────
// Allows existing code that tests or overrides OTA_GITHUB_REPO to keep
// compiling unchanged.
#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO REPO_OWNER_SLUG
#endif
