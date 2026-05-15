#pragma once

#include <stdint.h>

// ============================================================
//  BLE iBeacon scanner — venue room presence
//
//  Scans for iBeacon advertisements emitted by the venue's room
//  beacons. The room UID (loc code from floors.json:
//  floor_idx*100 + section_idx*10, off-site = 9xx) is carried in
//  the iBeacon major field (uint16 BE). The proximity UUID is a
//  fixed product identifier and is not parsed.
//
//  Lifecycle: the scanner is a singleton owned by main.cpp. The
//  MapScreen triggers begin()/startScan() on entry and stopScan()
//  on exit so the BT controller is only powered up while the user
//  is actually on the map.
// ============================================================

namespace BleBeaconScanner {

// Open a BLE session ASYNCHRONOUSLY: spawns a Core 0 helper task that
// shuts down WiFi (deinit + free its DRAM), initialises the BLE
// controller, and arms passive scanning. Returns immediately so the
// loop task can keep ticking the OLED contrast fade and GUI render
// pipeline. The status footer / BLE icon flip the moment the call
// returns; full BT init completes ~0.5-1 s later.
//
// Returns false if a previous session task is still in flight (the
// caller should treat this as transient and retry / no-op).
//
// Required because BLE controller (~30 KB) and WiFi LwIP stack (~50 KB)
// can't coexist in this build's internal DRAM budget.
bool beginSession();

// Bring up the BLE controller without arming a scan. Used by the badge
// advertiser, which only needs BLE initialised; MAP owns scan start/stop.
bool beginControllerOnly();

// Stop the active scan. The BLE controller intentionally stays up once
// initialised; repeated Arduino BLE deinit/reinit cycles have use-after-free
// hazards, and WiFi cannot be safely restarted while BLE owns internal DRAM.
void endSession();

// Best-effort full BLE shutdown for exclusive-takeover apps (e.g. Doom)
// that need every byte of contiguous internal DRAM. Stops scan, deinits
// the BLE host + controller, and releases the BT mode memory. Frees
// ~60 KB internal that the "controller stays up" policy normally
// preserves. After this call BLE is considered permanently dead until
// the next reboot — beginSession()/beginControllerOnly() will be no-ops.
// Safe to call multiple times; idempotent.
void shutdownForExclusiveApp();

// Convenience wrappers around the scan loop after beginSession() has
// brought the controller up. Idempotent.
void startScan();
void stopScan();

// Drop the BLEScan results vector so it doesn't grow unbounded during
// a long MAP session. Safe to call while scanning. MapScreen runs this
// from its 2 s heartbeat — without it, ~400 advs/s of accumulated
// BLEAdvertisedDevice objects starve the heap and break MAP exit.
void clearScanCache();

// Loop-level maintenance for an active BLE scan. Throttles clearScanCache()
// so MAP descendants/modals cannot leave Arduino-BLE's duplicate-result vector
// growing just because the overview screen is no longer on top.
void serviceScanCache();

// Reserve a contiguous internal-DRAM slab early in setup() so it is
// available for BLEDevice::init() later — without it, MicroPython /
// LwIP / TLS allocations fragment the internal heap so the largest
// free block drops below ~32 KB, which is the empirical floor for
// btdm_controller_init succeeding. The slab is freed inside
// beginSession() right before BT init. Once BLE is initialised the anchor is
// gone and WiFi is blocked from reconnect attempts until reboot.
// Returns true if the anchor was claimed; false if the heap was
// already too tight (caller may degrade gracefully).
bool reserveMemoryAnchor();

// Temporarily release the reserved internal-DRAM slab for memory-hungry
// WiFi/TLS work. Returns true when an anchor was released; callers that
// receive true should call reserveMemoryAnchor() again after the work.
bool releaseMemoryAnchor();

// True when BLE is armed, initialising, or initialised and WiFi should not
// try to allocate/reinitialise its radio stack. WiFiService checks this
// before and during connect retries so BLE and WiFi do not fight over the
// same internal DRAM window.
bool blocksWiFi();

// True if a beacon advertisement has been received within the freshness
// window (default 5 s).
bool hasFix();

// True from the moment startScan() arms the scan (even if init is still
// in flight) until stopScan() is called. The status header uses this
// to swap the WiFi icon for a BLE-scan glyph.
bool isScanning();

// Diagnostic counters used by MapScreen's 2-second heartbeat to prove
// the radio is actually receiving advertisements while a scan is armed.
// All counters are monotonic; subtract two reads to get a rate.
uint32_t advCount();
uint32_t iBeaconCount();
uint32_t authFailNoTime();      // dropped because NTP not synced
uint32_t authFailMismatch();    // matched format but UUID didn't match HMAC

// Diagnostic accessors for the auth gate's clock + cache state. epoch30
// is the badge's view of `unix_time / 30`; cachedEpoch30 is the value
// the UUID cache was computed against. When these drift apart the cache
// is stale and every freshly broadcast beacon will fail HMAC auth until
// the next refresh. Returns 0 when NTP isn't synced yet.
uint32_t currentEpoch30();
uint32_t cachedEpoch30();

// Returns the first 8 bytes of the most-recently observed iBeacon's
// proximity UUID. Empty (all-zero) until a frame is parsed. Useful to
// compare against the beacon's serial-printed UUID and confirm the
// HMAC inputs are aligned.
void copyLastObservedUuid(uint8_t out[8]);

// True once the underlying BLE controller has been initialised (i.e.
// after the first MAP-screen entry). Other subsystems that want to
// piggy-back on the existing controller — e.g. BadgeBeaconAdv —
// should gate themselves on this so they don't try to drive the
// controller before it's been brought up.
bool isInitialised();

// Compute the proximity UUID a venue beacon would broadcast for the
// given 30-second epoch window. Same HMAC scheme that the scanner's
// auth gate uses. Exposed so BadgeBeaconAdv can build its outgoing
// iBeacon advertisement with bytes matching the venue scheme.
void computeUuidForEpoch(uint32_t epoch30, uint8_t out[16]);
// iBeacons that passed the rotating-UUID HMAC auth — should track
// iBeaconCount() closely once a venue beacon is in range. A persistent
// gap (iBeaconCount > iBeaconAuthOkCount) means foreign / outdated
// iBeacons are nearby or the badge's clock isn't NTP-synced.
uint32_t iBeaconAuthOkCount();

// Strongest-RSSI room UID seen within the freshness window, or 0 if no
// fix. Decoded with floorForUid() / sectionForUid() below.
uint16_t bestRoomUid();

// Decode helpers. Match build-data.py's loc-code scheme:
//   floor_idx   = uid / 100              (off-site uses 9xx → floor 5)
//   section_idx = (uid / 10) % 10
// Both return -1 when uid == 0 / out of range.
int floorForUid(uint16_t uid);
int sectionForUid(uint16_t uid);

}  // namespace BleBeaconScanner
