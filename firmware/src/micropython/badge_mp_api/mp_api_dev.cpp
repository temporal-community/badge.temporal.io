#include <Arduino.h>
#include <WiFi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../identity/BadgeUID.h"
#include "../../hardware/Inputs.h"
#include "../../infra/BadgeConfig.h"
#include "../../ota/BadgeOTA.h"

#include "temporalbadge_runtime.h"

#if defined(BADGE_ENABLE_MP_DEV)

extern Inputs inputs;

// ── Dev harness (plan 02 Part A soak tests) ─────────────────────────────────
//
// One Python binding, one C dispatcher, many subcommands.  Called as:
//
//   badge.dev("btn", "up"|"down"|"left"|"right")
//   badge.dev("fb")                  # dumps OLED framebuffer rows as hex
//   badge.dev("wifi-scan")           # dumps nearby 2.4 GHz APs to serial
//   badge.dev("wifi-set", ssid, pass[, slot])  # updates a saved WiFi slot
//   badge.dev("ota-check")           # forces an OTA manifest check
//   badge.dev("uid")                 # returns the badge UUID
//
// The return value is always a short C string (stashed in a static
// buffer).  The Python side converts it to str and prints; NULL → "".

namespace {

char s_dev_result[64];

int dir_to_button_idx(const char *dir)
{
    if (!dir)
        return -1;
    if (strcmp(dir, "up") == 0 || strcmp(dir, "u") == 0)
        return 0;
    if (strcmp(dir, "down") == 0 || strcmp(dir, "d") == 0)
        return 1;
    if (strcmp(dir, "left") == 0 || strcmp(dir, "l") == 0)
        return 2;
    if (strcmp(dir, "right") == 0 || strcmp(dir, "r") == 0)
        return 3;
    char *end = nullptr;
    long v = strtol(dir, &end, 10);
    if (end && *end == '\0' && v >= 0 && v <= 3)
        return (int)v;
    return -1;
}

}  // namespace

extern "C" const char *temporalbadge_runtime_dev(int argc, const char **argv)
{
    s_dev_result[0] = '\0';
    if (argc <= 0 || !argv || !argv[0])
        return "";

    const char *cmd = argv[0];

    if (strcmp(cmd, "btn") == 0)
    {
        if (argc < 2)
            return "ERR: btn <up|down|left|right|0-3>";
        int idx = dir_to_button_idx(argv[1]);
        if (idx < 0)
            return "ERR: bad btn arg";
        inputs.injectPress((uint8_t)idx);
        snprintf(s_dev_result, sizeof(s_dev_result), "OK btn %d", idx);
        return s_dev_result;
    }

    if (strcmp(cmd, "fb") == 0)
    {
        // SSD1306 framebuffer is 8 pages × 128 bytes, one byte per
        // 8-pixel vertical column in each page.  We dump one page per
        // Serial line as 256 hex chars.  Consumer on the host can
        // unpack into an ASCII render or compare against a reference.
        int w = 0, h = 0, bytes = 0;
        const uint8_t *fb = temporalbadge_runtime_oled_get_framebuffer(
            &w, &h, &bytes);
        if (!fb || w != 128 || h != 64 || bytes != 1024)
        {
            return "ERR fb unexpected shape";
        }
        Serial.println("FB BEGIN");
        for (int page = 0; page < 8; ++page)
        {
            Serial.print("P");
            Serial.print(page);
            Serial.print(' ');
            for (int x = 0; x < 128; ++x)
            {
                char byte_s[3];
                snprintf(byte_s, sizeof(byte_s),
                         "%02x", fb[page * 128 + x]);
                Serial.print(byte_s);
            }
            Serial.println();
        }
        Serial.println("FB END");
        return "OK fb -> serial";
    }

    if (strcmp(cmd, "uid") == 0)
    {
        return uid_hex;
    }

    if (strcmp(cmd, "wifi-scan") == 0)
    {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, true);
        delay(100);
        const int count = WiFi.scanNetworks(/*async=*/false,
                                            /*hidden=*/true);
        Serial.printf("WIFI SCAN count=%d\n", count);
        for (int i = 0; i < count; ++i)
        {
            Serial.printf("AP ssid='%s' chan=%d rssi=%d auth=%d\n",
                          WiFi.SSID(i).c_str(),
                          WiFi.channel(i),
                          WiFi.RSSI(i),
                          static_cast<int>(WiFi.encryptionType(i)));
        }
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        return "OK wifi-scan -> serial";
    }

    if (strcmp(cmd, "wifi-set") == 0)
    {
        if (argc < 3)
            return "ERR: wifi-set <ssid> <pass> [slot]";
        uint8_t slot = 0;
        if (argc >= 4 && argv[3] && argv[3][0])
        {
            char *end = nullptr;
            long v = strtol(argv[3], &end, 10);
            if (!end || *end != '\0' || v < 0 ||
                v >= Config::kMaxWifiNetworks)
            {
                return "ERR: bad wifi slot";
            }
            slot = static_cast<uint8_t>(v);
        }
        badgeConfig.setWifiCredentialsAt(slot, argv[1], argv[2]);
        badgeConfig.save();
        snprintf(s_dev_result, sizeof(s_dev_result), "OK wifi slot %u",
                 static_cast<unsigned>(slot));
        return s_dev_result;
    }

    if (strcmp(cmd, "ota-check") == 0)
    {
        ota::CheckResult r = ota::checkNow(/*ignoreCooldown=*/true);
        snprintf(s_dev_result, sizeof(s_dev_result),
                 "OTA check=%u tag=%s err=%s",
                 static_cast<unsigned>(r),
                 ota::latestKnownTag(),
                 ota::lastErrorMessage());
        return s_dev_result;
    }

    if (strcmp(cmd, "help") == 0)
    {
        return "btn|fb|wifi-scan|wifi-set|ota-check|uid|help";
    }

    snprintf(s_dev_result, sizeof(s_dev_result),
             "ERR: unknown cmd \"%s\"", cmd);
    return s_dev_result;
}

#endif  // BADGE_ENABLE_MP_DEV
