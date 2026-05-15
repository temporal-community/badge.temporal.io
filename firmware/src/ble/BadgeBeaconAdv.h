#pragma once

#include <stdint.h>

// ============================================================
//  Badge-as-beacon advertiser
//
//  Periodically broadcasts a short iBeacon advertisement carrying
//  the same HMAC-SHA-256 proximity UUID that venue beacons use, so
//  any nearby tracker / phone / other badge can authenticate the
//  broadcast and decode it. Independent of which screen the user is
//  on — runs on its own Core 0 task and the radio time-slices with
//  whatever the scanner is doing.
//
//  Defaults: 30 s ± 10 s jitter between cycles, 5 s broadcast window.
//  Skipped only when:
//    - the BLE controller hasn't been brought up yet
//      (BleBeaconScanner::isInitialised() == false)
//    - WiFi is currently connected (don't add adv noise during HTTPS)
//
//  Memory: at boot we pre-allocate the BLEAdvertisementData objects,
//  the manufacturer-data String (capacity 25 B), and a small
//  "headroom" heap slab so the broadcast path doesn't have to fight
//  the scanner for contiguous internal DRAM mid-cycle.
// ============================================================

namespace BadgeBeaconAdv {

// Spawn the broadcaster task and pre-allocate its working buffers.
// Idempotent: subsequent calls are no-ops. Call once from main
// setup() after FreeRTOS is alive.
void begin();

// Foreground apps that need predictable internal heap (MicroPython games,
// Doom launch screens, etc.) can pause background badge advertising without
// changing IR ownership state.
void setPausedForForeground(bool paused);

// Boop/IR needs first claim on RMT DMA/internal heap. The Boop screen raises
// this pause before enabling IR so an in-flight BLE advert can stop early and
// no new advert starts while booping.
void setPausedForIr(bool paused);
bool isBroadcasting();

}  // namespace BadgeBeaconAdv
