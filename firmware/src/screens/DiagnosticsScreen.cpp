#include "DiagnosticsScreen.h"

#include <cstdio>
#include <WiFi.h>

#include "../identity/BadgeUID.h"
#include "../identity/BadgeVersion.h"
#include "../ui/GUI.h"
#include "../ui/OLEDLayout.h"
#include "../hardware/Haptics.h"
#include "../hardware/IMU.h"
#include "../hardware/Inputs.h"
#include "../hardware/Power.h"
#include "../hardware/oled.h"

void DiagnosticsScreen::render(oled& d, GUIManager& /*gui*/) {
  extern BatteryGauge batteryGauge;
  extern IMU imu;

  d.setFontPreset(FONT_TINY);
  d.setTextWrap(false);
  d.setDrawColor(1);

  char buf[32];

  d.setCursor(0, 0);
  d.print("DIAG  fw:" FIRMWARE_VERSION);
  OLEDLayout::drawHeaderRule(d, 9);

  d.setCursor(0, 11);
  d.print("UID:");
  d.print(uid_hex);

  d.setCursor(0, 20);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    d.print("WiFi: connected");
  } else {
    d.print("WiFi: ---");
  }

  d.setCursor(0, 29);
  if (wifiOk) {
    d.print("IP:");
    d.print(WiFi.localIP().toString());
  } else {
    d.print("IP: not connected");
  }

  d.setCursor(0, 38);
  if (batteryGauge.isReady()) {
#if defined(BADGE_ECHO)
    snprintf(buf, sizeof(buf), "B:%.2fV %.0f%% r%lu",
             batteryGauge.cellVoltage(),
             batteryGauge.stateOfChargePercent(),
             (unsigned long)batteryGauge.rawAdc());
#else
    snprintf(buf, sizeof(buf), "Batt:%.2fV %.0f%%",
             batteryGauge.cellVoltage(),
             batteryGauge.stateOfChargePercent());
#endif
  } else {
    snprintf(buf, sizeof(buf), "Batt: N/A");
  }
  d.print(buf);

  d.setCursor(0, 47);
#if defined(CHG_GOOD_PIN) && defined(CHG_STAT_PIN)
  {
    const Power::ChargerTelemetry telemetry = Power::readChargerTelemetry();
    snprintf(buf, sizeof(buf), "CHG:P%u S%u %s",
             (unsigned)telemetry.pgoodLevel,
             (unsigned)telemetry.statLevel,
             telemetry.state);
    d.print(buf);
  }
#else
  d.print("CHG: N/A");
#endif

  uint32_t upSec = millis() / 1000;
  d.setCursor(0, 56);
  snprintf(buf, sizeof(buf), "%s %luMHz %lum%02lus",
           imu.isReady() ? "IMU:OK" : "IMU:--",
           (unsigned long)getCpuFrequencyMhz(),
           (unsigned long)(upSec / 60),
           (unsigned long)(upSec % 60));
  d.print(buf);
}

void DiagnosticsScreen::handleInput(const Inputs& inputs, int16_t /*cx*/,
                                    int16_t /*cy*/, GUIManager& gui) {
  const Inputs::ButtonEdges& e = inputs.edges();
  if (e.cancelPressed) {
    Haptics::shortPulse();
    gui.replaceScreen(kScreenMainMenu);
  }
}
