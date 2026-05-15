#include "AssetLibraryScreen.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "../api/WiFiService.h"
#include "../hardware/Haptics.h"
#include "../hardware/Inputs.h"
#include "../hardware/oled.h"
#include "../ota/AssetRegistry.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"

namespace {

const ota::AssetEntry* sActiveAsset = nullptr;
char sQueuedSelectId[ota::kAssetIdMax] = "";

// Checkbox-style markers — the bracket pair reads as a clear "is this
// thing on my filesystem yet?" indicator on a single FONT_TINY row,
// without needing a custom glyph font:
//   [*]  installed (file/dir present on FFAT)
//   [+]  installed but the registry has a newer version
//   [!]  last install attempt failed
//   [ ]  not installed yet
const char* statusLabel(ota::AssetStatus s) {
  switch (s) {
    case ota::AssetStatus::kInstalled:       return "[*]";
    case ota::AssetStatus::kUpdateAvailable: return "[+]";
    case ota::AssetStatus::kFailed:          return "[!]";
    case ota::AssetStatus::kNotInstalled:
    default:                                 return "[ ]";
  }
}

}  // namespace

// ── AssetLibraryScreen ────────────────────────────────────────────────────

AssetLibraryScreen::AssetLibraryScreen()
    : ListMenuScreen(kScreenAssetLibrary, "COMMUNITY APPS") {}

void AssetLibraryScreen::selectAssetById(const char* id) {
  if (!id) { sQueuedSelectId[0] = '\0'; return; }
  std::strncpy(sQueuedSelectId, id, sizeof(sQueuedSelectId) - 1);
  sQueuedSelectId[sizeof(sQueuedSelectId) - 1] = '\0';
}

void AssetLibraryScreen::refreshStatusCache() {
  const uint8_t n = cachedCount_ < kStatusCacheCap ? cachedCount_
                                                   : kStatusCacheCap;
  pendingCount_ = 0;
  for (uint8_t i = 0; i < n; ++i) {
    const ota::AssetEntry* e = ota::registry::at(i);
    statusCache_[i] = e ? ota::registry::statusOf(*e)
                        : ota::AssetStatus::kNotInstalled;
    if (statusCache_[i] != ota::AssetStatus::kInstalled) ++pendingCount_;
  }
}

void AssetLibraryScreen::doRefresh(bool ignoreCooldown) {
  if (!wifiService.isConnected()) return;
  refreshing_ = true;
  // Force a redraw so the placeholder row flips to "Refreshing..." while
  // the synchronous HTTPS fetch holds the loop. The real refresh below
  // can take a few seconds on first connect.
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  ListMenuScreen::render(d, guiManager);
  d.sendBuffer();

  ota::registry::refresh(ignoreCooldown);
  cachedCount_ = static_cast<uint8_t>(ota::registry::count());
  refreshStatusCache();
  refreshing_ = false;
}

void AssetLibraryScreen::onEnter(GUIManager& gui) {
  ListMenuScreen::onEnter(gui);
  cachedCount_ = static_cast<uint8_t>(ota::registry::count());
  refreshStatusCache();

  // If the registry is empty whenever we land here, force-fetch (ignore
  // cooldown). Without ignoreCooldown a stale `last_epoch` from a prior
  // failed parse would leave the list permanently empty for 24h. Once
  // we have any assets cached, normal cooldown resumes via tick().
  if (cachedCount_ == 0 && wifiService.isConnected()) {
    doRefresh(/*ignoreCooldown=*/true);
  }

  // Honor a pending selectAssetById() from the DOOM no-WAD redirect.
  // Asset rows live at index (registry_idx + 1) because of the
  // synthetic "Download all" row at row 0.
  if (sQueuedSelectId[0]) {
    for (uint8_t i = 0; i < cachedCount_; ++i) {
      const ota::AssetEntry* e = ota::registry::at(i);
      if (e && std::strcmp(e->id, sQueuedSelectId) == 0) {
        cursor_ = static_cast<uint8_t>(i + 1);
        break;
      }
    }
    sQueuedSelectId[0] = '\0';
  } else if (cursor_ == 0 && cachedCount_ > 0) {
    // Default the cursor to the first real asset row instead of the
    // "Download all" row — the bulk action is opt-in, not opt-out.
    cursor_ = 1;
  }
}

void AssetLibraryScreen::handleInput(const Inputs& inputs, int16_t cx,
                                     int16_t cy, GUIManager& gui) {
  // X = manual refresh. Has to be handled here because ListMenuScreen's
  // base handleInput doesn't dispatch X. Eat the press before delegating
  // so the underlying joystick navigation still works.
  if (inputs.edges().xPressed) {
    Haptics::shortPulse();
    doRefresh(/*ignoreCooldown=*/true);
    return;
  }
  ListMenuScreen::handleInput(inputs, cx, cy, gui);
}

uint8_t AssetLibraryScreen::itemCount() const {
  // When the registry is empty we still expose one synthetic row so the
  // user gets actionable feedback ("No WiFi", "Refreshing...", etc.)
  // instead of a blank list. Otherwise we always expose a synthetic
  // "Download all" row at index 0 so the option is consistently
  // findable, even when everything is already installed (in that
  // case the row is informative rather than actionable).
  if (cachedCount_ == 0) return 1;
  return static_cast<uint8_t>(cachedCount_ + 1);
}

void AssetLibraryScreen::formatItem(uint8_t index, char* buf,
                                    uint8_t bufSize) const {
  if (cachedCount_ == 0) {
    if (refreshing_) {
      std::snprintf(buf, bufSize, "Refreshing...");
    } else if (!wifiService.isConnected()) {
      std::snprintf(buf, bufSize, "No WiFi - press X");
    } else {
      const char* err = ota::registry::lastErrorMessage();
      if (err && err[0]) {
        std::snprintf(buf, bufSize, "Empty: %s", err);
      } else {
        std::snprintf(buf, bufSize, "Empty - press X");
      }
    }
    return;
  }
  // Synthetic top row — always present.
  if (index == 0) {
    if (pendingCount_ == 0) {
      std::snprintf(buf, bufSize, ">>> All up to date");
    } else {
      std::snprintf(buf, bufSize, ">>> Download all (%u) >>>",
                    static_cast<unsigned>(pendingCount_));
    }
    return;
  }
  --index;  // shift past the synthetic row
  const ota::AssetEntry* e = ota::registry::at(index);
  if (!e) {
    std::snprintf(buf, bufSize, "(missing)");
    return;
  }
  // Use the per-row cache populated at refresh / enter / install time
  // instead of recomputing on every render frame.
  ota::AssetStatus s = (index < kStatusCacheCap)
                           ? statusCache_[index]
                           : ota::registry::statusOf(*e);
  std::snprintf(buf, bufSize, "%s %s", statusLabel(s), e->name);
}

namespace {
// Per-asset headline shown on the AssetLibraryScreen during a batch
// install. Rendered directly here (instead of pushing AssetDetail) so
// the batch flow is purely synchronous from the user's POV — no nested
// screen pop dance.
struct BatchUiCtx {
  oled* d;
  GUIManager* gui;
  AssetLibraryScreen* screen;
  char headline[32];
  size_t indexInBatch;
  size_t totalInBatch;
  size_t bytesWritten;
  size_t totalBytes;
};

void renderBatchProgress(BatchUiCtx& ctx) {
  oled& d = *ctx.d;
  d.clearBuffer();
  d.setDrawColor(1);
  OLEDLayout::drawStatusHeader(d, "INSTALLING ALL");
  d.setFontPreset(FONT_TINY);
  char line[40];
  std::snprintf(line, sizeof(line), "Asset %u / %u",
                static_cast<unsigned>(ctx.indexInBatch + 1),
                static_cast<unsigned>(ctx.totalInBatch));
  d.drawStr(2, 18, line);
  // Per-asset name, truncated to fit.
  char name[32];
  std::snprintf(name, sizeof(name), "%s", ctx.headline);
  OLEDLayout::fitText(d, name, sizeof(name), 124);
  d.drawStr(2, 27, name);

  if (ctx.totalBytes > 0) {
    std::snprintf(line, sizeof(line), "%u / %u KB",
                  static_cast<unsigned>(ctx.bytesWritten / 1024),
                  static_cast<unsigned>(ctx.totalBytes / 1024));
  } else {
    std::snprintf(line, sizeof(line), "%u KB",
                  static_cast<unsigned>(ctx.bytesWritten / 1024));
  }
  d.drawStr(2, 36, line);
  constexpr int kBarX = 4, kBarY = 42, kBarW = 120, kBarH = 8;
  d.drawRFrame(kBarX, kBarY, kBarW, kBarH, 1);
  if (ctx.totalBytes > 0) {
    int fill = static_cast<int>(((ctx.bytesWritten * (kBarW - 2)) /
                                 ctx.totalBytes));
    if (fill < 0) fill = 0;
    if (fill > kBarW - 2) fill = kBarW - 2;
    d.drawBox(kBarX + 1, kBarY + 1, fill, kBarH - 2);
  }
  OLEDLayout::drawNavFooter(d, "Do not unplug");
  d.sendBuffer();
}

void batchHeadlineCb(const ota::AssetEntry& entry, size_t i, size_t total,
                     void* user) {
  auto* ctx = static_cast<BatchUiCtx*>(user);
  if (!ctx) return;
  std::strncpy(ctx->headline, entry.name, sizeof(ctx->headline) - 1);
  ctx->headline[sizeof(ctx->headline) - 1] = '\0';
  ctx->indexInBatch = i;
  ctx->totalInBatch = total;
  ctx->bytesWritten = 0;
  ctx->totalBytes = entry.size;
  renderBatchProgress(*ctx);
}

void batchProgressCb(const ota::AssetProgress& prog, void* user) {
  auto* ctx = static_cast<BatchUiCtx*>(user);
  if (!ctx) return;
  ctx->bytesWritten = prog.bytesWritten;
  if (prog.totalBytes > 0) ctx->totalBytes = prog.totalBytes;
  // Throttle redraws — the progress callback fires up to 4x/sec from
  // installFileToPath; redraw at the same cadence.
  static uint32_t sLastDrawMs = 0;
  const uint32_t now = millis();
  if (!prog.done && (now - sLastDrawMs) < 200) return;
  sLastDrawMs = now;
  renderBatchProgress(*ctx);
}
}  // namespace

void AssetLibraryScreen::onItemSelect(uint8_t index, GUIManager& gui) {
  if (cachedCount_ == 0) {
    // Confirm on the placeholder row also forces a refresh — most users
    // tap A before reading the footer hint.
    Haptics::shortPulse();
    doRefresh(/*ignoreCooldown=*/true);
    return;
  }
  // Synthetic top row.
  if (index == 0) {
    Haptics::shortPulse();
    if (pendingCount_ == 0) {
      // Nothing to install — re-fetch the registry so the user can
      // confirm they actually have the latest list. Cheap and
      // reassuring vs. a silent confirm-with-no-effect.
      doRefresh(/*ignoreCooldown=*/true);
      return;
    }
    BatchUiCtx ctx{};
    ctx.d = &gui.oledDisplay();
    ctx.gui = &gui;
    ctx.screen = this;
    std::strncpy(ctx.headline, "Starting...", sizeof(ctx.headline) - 1);
    ctx.totalInBatch = pendingCount_;
    renderBatchProgress(ctx);
    ota::registry::installAll(&batchHeadlineCb, &batchProgressCb, &ctx);
    refreshStatusCache();
    return;
  }
  --index;  // shift past synthetic row
  const ota::AssetEntry* e = ota::registry::at(index);
  if (!e) return;
  AssetDetailScreen::setActiveAsset(e);
  Haptics::shortPulse();
  gui.pushScreen(kScreenAssetDetail);
}

// ── AssetDetailScreen ─────────────────────────────────────────────────────

void AssetDetailScreen::setActiveAsset(const ota::AssetEntry* entry) {
  sActiveAsset = entry;
}

void AssetDetailScreen::onEnter(GUIManager& /*gui*/) {
  phase_ = Phase::kIdle;
  bytesWritten_ = 0;
  totalBytes_ = sActiveAsset ? sActiveAsset->size : 0;
}

bool AssetDetailScreen::needsRender() {
  return phase_ == Phase::kInstalling;
}

void AssetDetailScreen::render(oled& d, GUIManager& /*gui*/) {
  d.setDrawColor(1);
  if (!sActiveAsset) {
    OLEDLayout::drawStatusHeader(d, "ASSET");
    d.setFontPreset(FONT_TINY);
    d.drawStr(2, 24, "(no asset selected)");
    OLEDLayout::drawNavFooter(d, "Cancel:Back");
    return;
  }

  OLEDLayout::drawStatusHeader(d, sActiveAsset->name);
  d.setFontPreset(FONT_TINY);

  if (phase_ == Phase::kInstalling) {
    d.drawStr(2, 22, "Downloading...");
    char szBuf[40];
    if (totalBytes_ > 0) {
      std::snprintf(szBuf, sizeof(szBuf), "%u / %u KB",
                    (unsigned)(bytesWritten_ / 1024),
                    (unsigned)(totalBytes_ / 1024));
    } else {
      std::snprintf(szBuf, sizeof(szBuf), "%u KB",
                    (unsigned)(bytesWritten_ / 1024));
    }
    d.drawStr(2, 31, szBuf);
    constexpr int kBarX = 4, kBarY = 38, kBarW = 120, kBarH = 8;
    d.drawRFrame(kBarX, kBarY, kBarW, kBarH, 1);
    if (totalBytes_ > 0) {
      int fill = static_cast<int>(((bytesWritten_ * (kBarW - 2)) /
                                   totalBytes_));
      if (fill < 0) fill = 0;
      if (fill > kBarW - 2) fill = kBarW - 2;
      d.drawBox(kBarX + 1, kBarY + 1, fill, kBarH - 2);
    }
    OLEDLayout::drawNavFooter(d, "Do not unplug");
    return;
  }

  if (phase_ == Phase::kError) {
    d.drawStr(2, 22, "Install failed:");
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s",
                  ota::registry::lastErrorMessage());
    OLEDLayout::fitText(d, buf, sizeof(buf), 124);
    d.drawStr(2, 32, buf);
    OLEDLayout::drawNavFooter(d, "Any:Back");
    return;
  }

  if (phase_ == Phase::kDone) {
    d.setFontPreset(FONT_SMALL);
    d.drawStr(2, 24, "Installed!");
    d.setFontPreset(FONT_TINY);
    d.drawStr(2, 36, sActiveAsset->dest_path);
    OLEDLayout::drawNavFooter(d, "Cancel:Back");
    return;
  }

  // Idle: show metadata + action footer.
  ota::AssetStatus status = ota::registry::statusOf(*sActiveAsset);
  char line[80];

  // Top line — installed version (or "not installed"). This is the
  // single most useful "is it on my filesystem yet?" datum, so it
  // gets the headline slot.
  const char* installed = ota::registry::installedVersionOf(*sActiveAsset);
  if (installed[0]) {
    if (status == ota::AssetStatus::kUpdateAvailable) {
      std::snprintf(line, sizeof(line), "On badge: %s (UPD)", installed);
    } else {
      std::snprintf(line, sizeof(line), "On badge: %s", installed);
    }
  } else {
    std::snprintf(line, sizeof(line), "On badge: (not installed)");
  }
  d.drawStr(2, 18, line);

  // Path it lives at on FFAT — directory for app bundles, file path
  // for single-file assets.
  const bool isApp = sActiveAsset->kind == ota::AssetKind::kApp;
  if (isApp && sActiveAsset->fileCount > 0) {
    std::snprintf(line, sizeof(line), "Path: %s/ (%u files)",
                  sActiveAsset->dest_path,
                  static_cast<unsigned>(sActiveAsset->fileCount));
  } else {
    std::snprintf(line, sizeof(line), "Path: %s",
                  sActiveAsset->dest_path);
  }
  OLEDLayout::fitText(d, line, sizeof(line), 124);
  d.drawStr(2, 27, line);

  // Latest version + size on one line so the description still fits.
  if (sActiveAsset->size > 0) {
    std::snprintf(line, sizeof(line), "Latest: %s  %u KB",
                  sActiveAsset->version,
                  static_cast<unsigned>(sActiveAsset->size / 1024));
  } else {
    std::snprintf(line, sizeof(line), "Latest: %s",
                  sActiveAsset->version);
  }
  OLEDLayout::fitText(d, line, sizeof(line), 124);
  d.drawStr(2, 36, line);

  if (sActiveAsset->description[0]) {
    char desc[80];
    std::snprintf(desc, sizeof(desc), "%s", sActiveAsset->description);
    OLEDLayout::fitText(d, desc, sizeof(desc), 124);
    d.drawStr(2, 45, desc);
  }

  const char* action = "Install";
  if (status == ota::AssetStatus::kInstalled) {
    action = "Remove";
  } else if (status == ota::AssetStatus::kUpdateAvailable) {
    action = "Update";
  }
  OLEDLayout::drawActionFooter(d, action, "Confirm");
}

void AssetDetailScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                    int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();

  if (phase_ == Phase::kInstalling) return;

  if (phase_ == Phase::kError || phase_ == Phase::kDone) {
    if (e.cancelPressed || e.confirmPressed || e.xPressed || e.yPressed) {
      phase_ = Phase::kIdle;
      gui.popScreen();
    }
    return;
  }

  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.popScreen();
    return;
  }

  if (!sActiveAsset) return;

  if (e.confirmPressed) {
    Haptics::shortPulse();
    ota::AssetStatus status = ota::registry::statusOf(*sActiveAsset);
    if (status == ota::AssetStatus::kInstalled) {
      runRemove(gui);
    } else {
      runInstall(gui);
    }
  }
}

void AssetDetailScreen::progressCb(const ota::AssetProgress& prog,
                                   void* user) {
  auto* self = static_cast<AssetDetailScreen*>(user);
  if (!self) return;
  self->bytesWritten_ = prog.bytesWritten;
  self->totalBytes_ = prog.totalBytes;
  if (prog.done) self->lastResult_ = prog.result;
  extern GUIManager guiManager;
  oled& d = guiManager.oledDisplay();
  d.clearBuffer();
  self->render(d, guiManager);
  d.sendBuffer();
}

void AssetDetailScreen::runInstall(GUIManager& gui) {
  if (!sActiveAsset) return;
  phase_ = Phase::kInstalling;
  bytesWritten_ = 0;
  totalBytes_ = sActiveAsset->size;

  oled& d = gui.oledDisplay();
  d.clearBuffer();
  render(d, gui);
  d.sendBuffer();

  const bool ok =
      ota::registry::install(*sActiveAsset, &AssetDetailScreen::progressCb,
                             this);
  if (ok) {
    phase_ = Phase::kDone;
  } else {
    phase_ = Phase::kError;
  }
}

void AssetDetailScreen::runRemove(GUIManager& /*gui*/) {
  if (!sActiveAsset) return;
  ota::registry::remove(*sActiveAsset);
  phase_ = Phase::kIdle;
}
