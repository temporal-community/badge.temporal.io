#include <Arduino.h>

#include "hardware/HardwareConfig.h"

#include "infra/BadgeConfig.h"
#include "infra/DebugLog.h"
#include "infra/Filesystem.h"
#include "micropython/StartupFiles.h"
#include "ui/GUI.h"
#include "ui/BootSplash.h"
#include "hardware/Haptics.h"
#include "hardware/IMU.h"
#include "hardware/Inputs.h"
#include "hardware/LEDmatrix.h"
#include "hardware/Power.h"
#include "infra/Scheduler.h"
#include "esp32-hal-gpio.h"
#include "hardware/oled.h"
#include <Wire.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "identity/BadgeUID.h"
#include "ui/BadgeDisplay.h"
#include "ir/BadgeIR.h"
#include "boops/BadgeBoops.h"
#include "identity/BadgeInfo.h"
#include "api/DataCache.h"
#include "api/WiFiService.h"
#ifdef BADGE_ENABLE_BLE_PROXIMITY
#include "ble/BadgeBeaconAdv.h"
#include "ble/BleBeaconScanner.h"
#endif
#include "led/LEDAppRuntime.h"
#include "micropython/MicroPythonMatrixService.h"
#include "infra/HeapDiag.h"
#include "ota/AssetRegistry.h"
#include "ota/BadgeOTA.h"
#include "ota/OTAService.h"

#ifdef BADGE_HAS_DOOM
#include "doom/doom_app.h"
#include "doom/doom_resources.h"
#endif

#include "hardware/PanicReset.h"

// Loop task stack: MicroPython, OLED composition, and optional hacker HTTP
// calls can still be stack-hungry, so keep the historical 24 KB headroom.
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

// ── Global hardware instances ─────────────────────────────────────────────────
// All peripheral globals are always declared. On hardware that lacks a
// peripheral, its class is a no-op stub (see HardwareConfig.h feature flags).
oled badgeDisplay;
Inputs inputs;
Scheduler scheduler;
SleepService sleepService;
IMU imu;
LEDmatrix badgeMatrix;
BatteryGauge batteryGauge;
HapticsService hapticsService;
FileBrowser fileBrowser;
GUIManager guiManager;
MicroPythonMatrixService microPythonMatrix;
TaskHandle_t gIrTaskHandle = NULL;

bool gWakeOnlyMode = false;

static constexpr size_t kMallocPreferInternalBelowBytes = 0;
static constexpr size_t kSerialRxBufferBytes = 4096;
static size_t sBootPsramBytes = 0;
static bool sBootPsramPolicyApplied = false;

static void configureBootHeapPolicy() {
    sBootPsramBytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if ( sBootPsramBytes == 0 ) return;

    heap_caps_malloc_extmem_enable( kMallocPreferInternalBelowBytes );
    sBootPsramPolicyApplied = true;
}

#ifdef BADGE_HAS_IMU
static void applyBadgeOrientation(BadgeOrientation orient) {
    if ( orient == BadgeOrientation::kUnknown ) {
        return;
    }

    // Keep interactive UI controls in the badge's normal orientation. An
    // inverted badge posture is only a passive display mode (nametag or other
    // asset), so it must not leak into joystick/button semantics when the
    // overlay is dismissed.
    badgeDisplay.setFlipped( false );
    inputs.setOrientation( BadgeOrientation::kUpright );
#ifdef BADGE_HAS_LED_MATRIX
    // Matrix stays fixed (no IMU-driven flip). Lock the alternate hardware
    // polarity so pixel coords match the mounted panel (see LEDmatrix::setFlipped).
    badgeMatrix.setFlipped( true );
#endif
}
#endif

extern "C" void mpy_start( Stream* stream ) __attribute__( ( weak ) );
extern "C" void mpy_poll( void ) __attribute__( ( weak ) );
extern "C" void mpy_pump_set_ready( void ) __attribute__( ( weak ) );

static void printReplayBootMarker(const char* phase) {
    Serial.printf("[boot] Temporal Replay 2026 - %s %s - phase=%s - boot_ms=%lu\n",
                  __DATE__, __TIME__, phase ? phase : "boot",
                  (unsigned long)millis());
}

void setup( ) {
    configureBootHeapPolicy();
    Serial.setRxBufferSize( kSerialRxBufferBytes );
    Serial.begin( 115200 );
    printReplayBootMarker("serial");
    Serial.println( "Starting..." );
    if ( sBootPsramPolicyApplied ) {
        Serial.printf( "[boot] PSRAM heap=%u bytes; malloc prefers internal below %u bytes\n",
                       (unsigned)sBootPsramBytes,
                       (unsigned)kMallocPreferInternalBelowBytes );
    } else {
        Serial.println( "[boot] PSRAM heap unavailable; using default malloc policy" );
    }
    // Diagnostic: print prior reset reason so a silent reset on the
    // previous boot identifies itself (panic / task WDT / brownout / etc).
    Serial.printf( "[boot] reset_reason=%d (1=PWR 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 11=RTC 15=BROWNOUT)\n",
                   (int)esp_reset_reason() );
    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause( );
    gWakeOnlyMode = ( wakeCause == ESP_SLEEP_WAKEUP_TIMER );
    Serial.printf( "Wake cause: %d, wake-only mode: %d\n", (int)wakeCause, gWakeOnlyMode ? 1 : 0 );

    Power::applyBootRadioDefaults( );
    Power::initCpuGovernor( );

    // Single, authoritative I2C master init. All on-bus drivers (IMU, OLED,
    // LED matrix, battery gauge) share Wire — they must not call Wire.begin
    // themselves, and nothing downstream should call Wire.end()/Wire.begin()
    // once drivers are live. The Arduino wrapper silences IDF i2c.master
    // logs inside i2cInit(); re-enable them so a real NACK / timeout is
    // visible instead of the generic ESP_ERR_INVALID_STATE(259).
    Wire.begin( SDA_PIN, SCL_PIN, 400000 );
    Wire.setTimeOut( 200 );
    esp_log_level_set( "i2c.master", ESP_LOG_ERROR );

#ifdef BADGE_HAS_SLEEP_SERVICE
    // SYSOFF is active-high on the charger path; never enable a pull-up on it.
#if defined(SYSOFF_PIN)
    digitalWrite(SYSOFF_PIN, LOW);
    pinMode(SYSOFF_PIN, OUTPUT);
#endif
#if !defined(SYSOFF_PIN) || (MCU_SLEEP_PIN != SYSOFF_PIN)
    pinMode(MCU_SLEEP_PIN, INPUT_PULLUP);
#endif
#if !defined(SYSOFF_PIN) || (INT_GP_PIN != SYSOFF_PIN)
    pinMode(INT_GP_PIN, INPUT_PULLUP);
#endif
#endif

// pinMode(IR_TX_PIN, OUTPUT);
// pinMode(IR_RX_PIN, INPUT);

// for (int i = 0; i < 500; i++) {
//     Serial.printf("Sending IR frame %d\n", i);
//     digitalWrite(IR_TX_PIN, HIGH);
//     Serial.printf("IR RX: %d\n", digitalRead(IR_RX_PIN));
//     delay(100);
//     digitalWrite(IR_TX_PIN, LOW);
//     Serial.printf("IR RX: %d\n", digitalRead(IR_RX_PIN));
//     delay(100);
// }

    // UID must be populated before badgeConfig.load() because
    // loadStringsFromNvs() unscrambles the WiFi password using a key
    // derived from `uid[]`. Loading first would XOR the saved blob
    // against zero-uid garbage, producing an unusable password and
    // making auto-connect fail silently on every cold boot.
    read_uid();
    Serial.printf( "  UID: %s\n", uid_hex );
    badgeConfig.load();
    displayInit();
#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BleBeaconScanner::reserveMemoryAnchor();
#endif

    // Brownout-sensitive peripherals (haptics motor, IMU, LED matrix
    // power-on inrush, IR transmitter) come up later from
    // initDeferredPeripherals(), called the first time the home
    // screen is entered. Pre-home rendering only needs the OLED.

    // if ( gWakeOnlyMode ) {
    //     scheduler.setExecutionDivisors( 1, 1, 1 );
    //     Serial.println( "Wake-only mode active" );
    //     return;
    // }

    // ── ESP-IDF networking prerequisites ────────────────────────────────────
    // MicroPython's network.WLAN, socket, ssl, espnow and bluetooth modules
    // all expect the IDF event loop, NVS, and esp_netif to be initialised
    // before the runtime starts. arduino-esp32's WiFi.begin() does this
    // lazily, but MicroPython users expect to be able to instantiate
    // network.WLAN() directly from the REPL. Initialise here so the surface
    // is available unconditionally — these calls are idempotent and cheap.
    {
        esp_err_t nvsRc = nvs_flash_init();
        if ( nvsRc == ESP_ERR_NVS_NO_FREE_PAGES ||
             nvsRc == ESP_ERR_NVS_NEW_VERSION_FOUND ) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        esp_netif_init();
        esp_event_loop_create_default();
    }

    // ── MicroPython + FAT mount ───────────────────────────────────────────────
    if ( mpy_start != nullptr ) {
        mpy_start( &Serial );
    } else {
        Serial.println( "MicroPython not linked" );
    }
    HeapDiag::printSnapshot("post-mpy-init");

    provisionStartupFiles(); // set to true to force filesystem rebuild
    BadgeBoops::begin();

    // Schedule / speakers / floors content cache. Synchronous: tries
    // /data.bin from FatFS, falls back to the bundle linked via embed_files.
    DataCache::init(nullptr);

    {
        BadgeInfo::Fields info;
        if (!BadgeInfo::loadFromFile(info)) {
            BadgeInfo::populateDefaults(info, uid);
            BadgeInfo::saveToFile(info);
            Serial.println("BadgeInfo: created /badgeInfo.json");
        }
        BadgeInfo::applyToGlobals(info);
    }


    if ( badgeConfig.loadFromFile( ) ) {
        badgeConfig.applyAll( );
    }

    extern BadgeState badgeState;
    badgeState = BADGE_PAIRED;
    Serial.println( "[boot] offline mode active — pairing gate disabled" );

    // ── OLED init ─────────────────────────────────────────────────────────────
    const bool oled_ok = ( badgeDisplay.init( ) == 0 );
    if ( oled_ok ) {
        // Clamp boot-screen contrast to 150 to keep the OLED's load
        // off the 3V3 rail before the haptics/LED-matrix/IMU/IR
        // peripherals come up. initDeferredPeripherals() restores the
        // user's preferred contrast (via badgeConfig.applyAll) once
        // the home screen is reached.
        badgeDisplay.setContrast( 80 );
        badgeDisplay.clearDisplay( );
        badgeDisplay.setCursor( 0, 0 );
#ifdef BADGE_HAS_SLEEP_SERVICE
        badgeDisplay.bindPin("int_gp", INT_GP_PIN);
#endif
        Serial.println( "OLED init OK (contrast clamped to 150 for boot)" );
    } else {
        Serial.println( "OLED init failed — continuing without display" );
    }

    PanicReset::begin(oled_ok ? &badgeDisplay : nullptr);

    // Pre-splash screen is intentionally blank — BootSplash is the only
    // boot visual. Keeping the panel cleared (and contrast clamped to
    // 80 from the init block above) so the splash's hardware fade-in
    // starts from true black.
    badgeDisplay.clearBuffer();
    badgeDisplay.sendBuffer();
    Serial.println( "=== BADGE MERGE CODE RUNNING ===" );
    Serial.println( "  Network:   explicit-only MicroPython helpers" );

    // Boot splash — temporal-logo starfield. Blocks setup() for the duration;
    // ends with transitionOut(300) so the panel is dark when GUIManager
    // pushes the home screen. Bring up just the LED matrix's I2C controller
    // first so the splash can drive its sparkle animation; full service
    // registration stays in initDeferredPeripherals().
#ifdef BADGE_HAS_LED_MATRIX
    if ( badgeMatrix.init( LED_MATRIX_I2C_ADDRESS ) == 0 ) {
        badgeMatrix.setBrightness( Power::Policy::kLedMatrixDefaultBrightness );
        badgeMatrix.setFlipped( true );
        badgeMatrix.clear( 0 );
        Serial.println( "LED matrix init OK (early, for splash sparkles)" );
    } else {
        Serial.println( "LED matrix init failed (early); splash sparkles disabled" );
    }
#endif

    if ( oled_ok ) {
        BootSplash::playStarfield(4000, 60);
    }

    #ifndef BADGE_HAS_IMU
    badgeDisplay.setFlipped( false );
#endif

    badgeDisplayActive = true;
    screenDirty = true;
    printReplayBootMarker("ready");
    Serial.println( "=== offline boot complete ===" );

    // LED matrix init deferred to initDeferredPeripherals() — the
    // power-on inrush is the worst single brownout offender.

    inputs.begin( );
    Serial.println( "Input handler init OK" );

#ifdef BADGE_HAS_BATTERY_GAUGE
    const bool gauge_ok = batteryGauge.begin( );
    if ( gauge_ok ) {
        Serial.printf( "Battery: %.1f%%  %.3f V\n",
                       batteryGauge.stateOfChargePercent( ),
                       batteryGauge.cellVoltage( ) );
    } else {
        Serial.println( "Battery gauge init failed" );
    }
#endif

    // LED matrix / IMU bindings deferred — they reference peripherals
    // that initDeferredPeripherals() will bring up.
    badgeDisplay.bindInputs( &inputs );
#ifdef BADGE_HAS_BATTERY_GAUGE
    badgeDisplay.bindBatteryGauge( &batteryGauge );
#endif
#ifdef BADGE_HAS_HAPTICS
    hapticsService.bindDisplay( &badgeDisplay );
    hapticsService.bindInputs( &inputs );
#endif

    badgeConfig.applyAll();
    // applyAll may have written the user's preferred contrast — re-
    // clamp to 150 so we ride out the rest of boot at the safer level.
    if ( oled_ok ) {
        badgeDisplay.setContrast( 150 );
    }

    // IMU orientation detection deferred — it depends on imu.begin().




    if ( mpy_pump_set_ready != nullptr ) {
        mpy_pump_set_ready( );
    }

    fileBrowser.begin( &badgeDisplay, &inputs, &badgeConfig );

    scheduler.registerService( &inputs, ServicePriority::kHigh );

#ifdef BADGE_HAS_HAPTICS
    scheduler.registerService( &hapticsService, ServicePriority::kHigh );
#endif

    // LED-matrix services deferred — they're registered from
    // initDeferredPeripherals() once the matrix is powered on.

    scheduler.registerService( &fileBrowser, ServicePriority::kNormal );
    scheduler.registerService( &badgeDisplay, ServicePriority::kNormal );

#ifdef BADGE_HAS_BATTERY_GAUGE
    if ( gauge_ok ) {
        scheduler.registerService( &batteryGauge, ServicePriority::kLow );
    }
#endif

    // Boot/pairing rendering is complete. From here, GUIManager owns OLED
    // rendering and disables the legacy OLED scheduler service in begin().
    badgeDisplayActive = false;
    renderMode = MODE_MENU;
    screenDirty = false;
    guiManager.begin( &badgeDisplay, &inputs );
    // Nametag-mode set deferred — needs imu.getOrientation().
    scheduler.registerService( &guiManager, ServicePriority::kHigh );

    wifiService.begin();
    scheduler.registerService( &wifiService, ServicePriority::kLow );
    HeapDiag::printSnapshot("post-wifi-begin");

    // OTA + asset registry. begin() loads NVS caches and detects
    // ESP_OTA_IMG_PENDING_VERIFY (first boot after a fresh install).
    // OTAService runs at low priority and forwards a daily-cadence
    // tick to BadgeOTA + AssetRegistry.
    ota::begin();
    ota::registry::begin();
    scheduler.registerService( &ota::otaService, ServicePriority::kLow );
    HeapDiag::printSnapshot("post-ota-begin");

    // IR task creation deferred — initDeferredPeripherals() spawns it
    // after the home screen is reached, so the IR transmitter pulses
    // can't brown out the rail during boot.

    scheduler.setExecutionDivisors( Power::Policy::schedulerHighDivisor,
                                    Power::Policy::schedulerNormalDivisor,
                                    Power::Policy::schedulerLowDivisor );


    configWatcher.begin( &badgeConfig );
    scheduler.registerService( &configWatcher, ServicePriority::kLow );

#ifdef BADGE_ENABLE_BLE_PROXIMITY
    BadgeBeaconAdv::begin();
#endif
    HeapDiag::printSnapshot("setup-complete");
    HeapDiag::printRegionMap("setup-complete");
}

// Brings up the brownout-sensitive peripherals deferred from setup():
// haptics motor, IMU, LED matrix, IR task. Called the first time the
// home grid menu is entered (see GridMenuScreen::onEnter), guarded by
// a static flag so re-entries are no-ops. Once everything is online,
// re-applies badgeConfig so the user's preferred OLED contrast (which
// boot clamped to 150) is restored.
extern "C" void initDeferredPeripherals() {
    static bool done = false;
    if ( done ) return;
    done = true;

    Serial.println( "=== initDeferredPeripherals: bringing peripherals online ===" );

#ifdef BADGE_HAS_HAPTICS
    Haptics::begin();
#endif

#ifdef BADGE_HAS_IMU
    const bool imu_ok = imu.begin( );
    Serial.println( imu_ok ? "IMU init OK (deferred)"
                           : "IMU init failed (deferred)" );
    if ( imu_ok ) {
        scheduler.registerService( &imu, ServicePriority::kHigh );
        sleepService.begin( &imu, gWakeOnlyMode ? nullptr : &badgeMatrix,
                            gWakeOnlyMode ? nullptr : &badgeDisplay,
                            INT_GP_PIN,
                            Power::Policy::kLightSleepAfterNoMotionMs,
                            Power::Policy::kDeepSleepAfterNoMotionMs );
        scheduler.registerService( &sleepService, ServicePriority::kLow );
#ifdef BADGE_HAS_SLEEP_SERVICE
        sleepService.bindInputs( &inputs );
#endif
        badgeDisplay.bindAccel( &imu );

        imu.checkInitialOrientation( );
        if ( imu.flipChanged( ) ) {
            applyBadgeOrientation( imu.getOrientation( ) );
            imu.clearFlipChanged( );
        }
        if ( imu.getOrientation( ) != BadgeOrientation::kUnknown ) {
            guiManager.setNametagMode(
                imu.getOrientation( ) == BadgeOrientation::kInverted );
        }
    }
#endif

#ifdef BADGE_HAS_LED_MATRIX
    if ( badgeMatrix.init( LED_MATRIX_I2C_ADDRESS ) != 0 ) {
        Serial.println( "LED matrix init failed (deferred)" );
    } else {
        badgeMatrix.setBrightness( Power::Policy::kLedMatrixDefaultBrightness );
        badgeMatrix.setFlipped( true );
        badgeMatrix.clear( 0 );
        Serial.println( "LED matrix init OK (deferred)" );
    }
    badgeMatrix.bindInputs( &inputs );
#ifdef BADGE_HAS_IMU
    badgeMatrix.bindAccel( &imu );
#endif
    scheduler.registerService( &badgeMatrix,        ServicePriority::kHigh );
    scheduler.registerService( &microPythonMatrix,  ServicePriority::kHigh );
    ledAppRuntime.begin( &badgeMatrix );
    scheduler.registerService( &ledAppRuntime,      ServicePriority::kNormal );
#endif

    BaseType_t irTaskStatus =
        xTaskCreatePinnedToCore( irTask, "IR", 8192, NULL, 1, &gIrTaskHandle, 0 );
    if ( irTaskStatus != pdPASS ) {
        gIrTaskHandle = NULL;
        Serial.println( "Failed to create IR task; IR will be unavailable." );
    }

#ifdef BADGE_ENABLE_BLE_PROXIMITY
    // BLE init is intentionally NOT done here. MapScreen owns proximity
    // startup when BADGE_ENABLE_BLE_PROXIMITY is explicitly enabled.
#endif

    // Restore user-preferred OLED contrast now that we're past the
    // brownout-sensitive boot window.
    badgeConfig.applyAll();
}

void loop( ) {
#ifdef BADGE_HAS_DOOM
    const bool doomOwnsBadge = !gWakeOnlyMode && doom_resources_active();
    if ( !doomOwnsBadge ) {
        scheduler.runOnce( );
    }
#else
    scheduler.runOnce( );
#endif

    if ( !gWakeOnlyMode ) {
#ifdef BADGE_HAS_DOOM
        if ( doom_resources_active() ) {
            if ( !doom_app_is_running() ) {
                doom_app_exit();
                doom_app_deinit();
                doom_resources_exit();

                Haptics::off();

                // Wait for the user to release all buttons (they were
                // holding L+R for the exit combo) before restarting input.
                // Doom detaches input interrupts while it runs, so first
                // resync the logical state with the physical pins before
                // waiting; otherwise a launch/exit button can stay logically
                // held and repeat into the restored menu.
                inputs.resyncButtons();
                DBG("[doom] waiting for input release before GUI restore\n");

                // Spin until all buttons are released so the exit combo
                // doesn't leak into the GUI as spurious presses.
                {
                    uint32_t releaseDeadline = millis() + 2000;
                    bool allUp = false;
                    while ( !allUp && millis() < releaseDeadline ) {
                        inputs.update();
                        inputs.clearEdges();
                        const Inputs::ButtonStates& b = inputs.buttons();
                        allUp = !b.up && !b.down && !b.left && !b.right;
                        inputs.service();
                        delay(20);
                    }
                    if (!allUp) {
                        DBG("[doom] input release wait timed out\n");
                    }
                    // One final flush after release
                    uint32_t quietUntil = millis() + 250;
                    while (millis() < quietUntil) {
                        inputs.update();
                        inputs.clearEdges();
                        delay(20);
                    }
                }

                guiManager.activate();
                DBG("[doom] teardown complete, GUI restored\n");
            } else {
                Power::noteActivity();
                delay(1);
            }
            return;
        }
#endif

        if ( guiManager.isActive() ) {
            guiManager.handleInputIfActive();
        }
#ifdef BADGE_HAS_DOOM
        if ( doom_resources_active() ) {
            Power::noteActivity();
            delay(1);
            return;
        }
#endif
        inputs.clearEdges();
    }

#ifdef BADGE_HAS_IMU
    if ( imu.flipChanged( ) ) {
        const auto orient = imu.getOrientation( );
        DBG("imu.tiltXMag: %f, imu.tiltYMag: %f\n", imu.tiltXMg(), imu.tiltYMg());
        
        applyBadgeOrientation( orient );
        guiManager.setNametagMode( orient == BadgeOrientation::kInverted );
        imu.clearFlipChanged( );
    }
#endif

#ifdef BADGE_HAS_LED_MATRIX
    if ( !gWakeOnlyMode && LEDmatrixAnimator::isUserActive( inputs ) ) {
        Power::noteActivity( );
    }
#endif
#ifdef BADGE_HAS_SLEEP_SERVICE
    sleepService.processDeferredWake( );
#endif

    Power::updateCpuGovernor( );

#ifdef BADGE_ENABLE_BLE_PROXIMITY
    if ( !gWakeOnlyMode ) {
        BleBeaconScanner::serviceScanCache();
    }
#endif

    if ( !gWakeOnlyMode && mpy_poll != nullptr ) {
         mpy_poll( );
    }
    Power::applyLoopPacing( );
}
