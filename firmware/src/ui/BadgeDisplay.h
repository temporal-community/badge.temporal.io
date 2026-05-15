// BadgeDisplay.h — Public interface for the BadgeDisplay module
// Adapted from modular firmware to use the oled class (Adafruit SSD1306)
// instead of U8G2.

#pragma once
#include <Arduino.h>

class oled;

// ─── Display mutex (thread safety for Core 0 probe task) ──────────────────────
extern SemaphoreHandle_t displayMutex;

#define DISPLAY_TAKE()  xSemaphoreTake(displayMutex, portMAX_DELAY)
#define DISPLAY_GIVE()  xSemaphoreGive(displayMutex)

// ─── Badge state (shared across modules) ─────────────────────────────────────
//
// Lifecycle is now offline-first. BADGE_PAIRED is set at boot so the menu is
// immediately available; the other values remain for compatibility with older
// modules and saved state.
//
// The numeric values matter: GUIManager casts to uint8_t for change
// detection (`routedBadgeState_`). Keep BADGE_UNPAIRED at 0 so the
// default-initialized flag matches the boot-time state.
enum BadgeState : uint8_t {
  BADGE_UNPAIRED = 0,
  BADGE_PAIRED   = 1,
  BADGE_OFFLINE  = 2,
};

// Compatibility helper for code that still speaks in old activation terms.
inline bool badgeIsActivated(BadgeState s) {
  return s == BADGE_PAIRED || s == BADGE_OFFLINE;
}

// Compatibility helper for explicit networking status.
void setBadgeOnline(bool online);

const char* badgeStateName(BadgeState s);

// ─── Screen state (shared with main sketch and BadgeIR) ──────────────────────
extern String screenLine1;
extern String screenLine2;
extern bool   screenDirty;

enum RenderMode { MODE_BOOT, MODE_QR, MODE_BOOP, MODE_MENU, MODE_INPUT_TEST, MODE_BOOP_RESULT, MODE_INFO };
extern RenderMode     renderMode;
extern unsigned long  inputTestLastActivity;
extern unsigned long  boopResultShownAt;

// True when legacy direct display rendering owns the screen.
extern bool badgeDisplayActive;

// ─── Display control ─────────────────────────────────────────────────────────

void displayInit();
void setDisplayFlip(bool flip);
void setScreenText(const char* line1, const char* line2);

// ─── Render functions (Core 1 only, hold mutex) ───────────────────────────────

void renderScreen();
void bootPrint(const char* msg);
void drawReplayBootFrame(oled& d, uint8_t frame);
uint8_t replayBootFrameCount();
uint16_t replayBootFrameMs();
void drawReplayBootFinal(oled& d);
void tickBootAnimation();
void bootAnimationDelay(uint32_t durationMs);
void showBootFinalFrame();
void completeBootAnimation();

// ─── Drawing helpers ─────────────────────────────────────────────────────────

void drawXBM(int x, int y, int w, int h, const uint8_t* bits);
void drawStringCharWrap(int x, int y, int maxWidth, int lineHeight, const char* str);
