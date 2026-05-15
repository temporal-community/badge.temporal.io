#include <Arduino.h>
#include <string.h>

#include "../../infra/Bitops.h"
#include "../../hardware/oled.h"
#include "../../ui/AppIcons.h"
#include "../../ui/ButtonGlyphs.h"
#include "../../ui/OLEDLayout.h"
#include "../../ui/UIFonts.h"

#include "Internal.h"
#include "temporalbadge_runtime.h"

extern oled badgeDisplay;

// ── OLED -- text ────────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_init(void)
{
    return 0;
}

extern "C" int temporalbadge_runtime_oled_print(const char *msg)
{
    if (!msg)
    {
        return 0;
    }
    mpy_oled_note_activity();
    badgeDisplay.print(msg);
    return 0;
}

extern "C" int temporalbadge_runtime_oled_println(const char *msg)
{
    if (!msg)
    {
        return 0;
    }
    mpy_oled_note_activity();
    badgeDisplay.println(msg);
    mp_mouse_overlay_composite();
    badgeDisplay.display();
    mp_mouse_overlay_restore();
    return 0;
}

extern "C" int temporalbadge_runtime_oled_clear(int show)
{
    mpy_oled_note_activity();
    badgeDisplay.clearDisplay();
    badgeDisplay.setCursor(0, 0);
    if (show)
    {
        mp_mouse_overlay_composite();
        badgeDisplay.display();
        mp_mouse_overlay_restore();
    }
    return 0;
}

extern "C" int temporalbadge_runtime_oled_show(void)
{
    mpy_oled_note_activity();
    mp_mouse_overlay_composite();
    badgeDisplay.display();
    mp_mouse_overlay_restore();
    return 0;
}

extern "C" int temporalbadge_runtime_oled_set_cursor(int x, int y)
{
    mpy_oled_note_activity();
    badgeDisplay.setCursor(x, y);
    return 0;
}

extern "C" int temporalbadge_runtime_oled_set_text_size(int size)
{
    if (size < 1)
        size = 1;
    if (size > 4)
        size = 4;
    badgeDisplay.setTextSize((uint8_t)size);
    return 1;
}

extern "C" int temporalbadge_runtime_oled_get_text_size(void)
{
    return badgeDisplay.getTextSize();
}

extern "C" int temporalbadge_runtime_oled_invert(int invert)
{
    mpy_oled_note_activity();
    badgeDisplay.invertDisplay(invert != 0);
    return 0;
}

// ── OLED -- text metrics ────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_oled_text_width(const char *text)
{
    if (!text)
        return 0;
    return badgeDisplay.getStrWidth(text);
}

extern "C" int temporalbadge_runtime_oled_text_height(void)
{
    return badgeDisplay.getMaxCharHeight();
}

// ── OLED -- fonts ───────────────────────────────────────────────────────────

extern "C" int temporalbadge_runtime_oled_set_font(const char *name)
{
    return badgeDisplay.setFont(name) ? 1 : 0;
}

extern "C" const char *temporalbadge_runtime_oled_get_fonts(void)
{
    static char buf[768];
    buf[0] = '\0';
    for (int i = 0; i < kReplayFontCount; ++i)
    {
        if (i > 0)
        {
            strlcat(buf, ",", sizeof(buf));
        }
        strlcat(buf, kReplayFonts[i].name, sizeof(buf));
    }
    return buf;
}

extern "C" const char *temporalbadge_runtime_oled_get_current_font(void)
{
    return badgeDisplay.getFontName();
}

// ── OLED -- framebuffer / pixel ─────────────────────────────────────────────

extern "C" int temporalbadge_runtime_oled_set_pixel(int x, int y, int color)
{
    mpy_oled_note_activity();
    badgeDisplay.setDrawColor(color ? 1 : 0);
    badgeDisplay.drawPixel(x, y);
    badgeDisplay.setDrawColor(1);
    return 1;
}

extern "C" int temporalbadge_runtime_oled_get_pixel(int x, int y)
{
    return badgeDisplay.getPixel(x, y) ? 1 : 0;
}

extern "C" void temporalbadge_runtime_oled_draw_box(int x, int y, int w, int h)
{
    mpy_oled_note_activity();
    badgeDisplay.drawBox(x, y, w, h);
}

extern "C" void temporalbadge_runtime_oled_set_draw_color(int color)
{
    badgeDisplay.setDrawColor((uint8_t)(color & 3));
}

extern "C" const uint8_t *temporalbadge_runtime_oled_get_framebuffer(
    int *w, int *h, int *buf_size)
{
    *w = badgeDisplay.width();
    *h = badgeDisplay.height();
    *buf_size = (*w * *h) / 8;
    return badgeDisplay.getBufferPtr();
}

namespace {

void rotateFramebuffer180(uint8_t *buf, int total)
{
    using bitops::reverseBits;
    int lo = 0, hi = total - 1;
    while (lo < hi)
    {
        uint8_t a = reverseBits(buf[lo]);
        uint8_t b = reverseBits(buf[hi]);
        buf[lo] = b;
        buf[hi] = a;
        ++lo;
        --hi;
    }
    if (lo == hi)
    {
        buf[lo] = reverseBits(buf[lo]);
    }
}

}  // namespace

extern "C" int temporalbadge_runtime_oled_set_framebuffer(
    const uint8_t *data, size_t len)
{
    const int dw = badgeDisplay.width();
    const int dh = badgeDisplay.height();
    const int expected = (dw * dh) / 8;

    if (len == 0 || (int)len > expected)
    {
        Serial.printf("[mpy] oled_set_framebuffer: got %d bytes, max %d\n",
                      (int)len, expected);
        return 0;
    }

    mpy_oled_note_activity();
    uint8_t *buffer = badgeDisplay.getBufferPtr();

    if ((int)len == expected)
    {
        memcpy(buffer, data, len);
    }
    else if (len % (size_t)dw == 0)
    {
        int src_pages = (int)len / dw;
        int dst_pages = dh / 8;
        int page_offset = (dst_pages - src_pages) / 2;
        memset(buffer, 0, expected);
        memcpy(buffer + page_offset * dw, data, len);
    }
    else
    {
        Serial.printf("[mpy] oled_set_framebuffer: %d bytes not a multiple of width %d\n",
                      (int)len, dw);
        return 0;
    }

    rotateFramebuffer180(buffer, expected);
    mp_mouse_overlay_composite();
    badgeDisplay.display();
    mp_mouse_overlay_restore();
    return 1;
}

extern "C" void temporalbadge_runtime_oled_get_framebuffer_size(
    int *w, int *h, int *buf_bytes)
{
    *w = badgeDisplay.width();
    *h = badgeDisplay.height();
    *buf_bytes = (*w * *h) / 8;
}

namespace {

constexpr int kMpyFooterTopY = 53;
constexpr int kMpyFooterBaseY = 62;

void makeActionHint(char *out, size_t out_cap, const char *button,
                    const char *label)
{
    if (!out || out_cap == 0)
    {
        return;
    }
    out[0] = '\0';
    if (!button || !button[0])
    {
        return;
    }
    if (label && label[0])
    {
        snprintf(out, out_cap, "%s:%s", button, label);
    }
    else
    {
        snprintf(out, out_cap, "%s", button);
    }
}

void prepareUiDraw()
{
    badgeDisplay.setFont(UIFonts::kText);
    badgeDisplay.setDrawColor(1);
}

void drawActionBar(const char *left_button, const char *left_label,
                   const char *right_button, const char *right_label)
{
    prepareUiDraw();
    badgeDisplay.drawHLine(0, kMpyFooterTopY, OLEDLayout::kScreenW);

    char hint[32] = {};
    makeActionHint(hint, sizeof(hint), left_button, left_label);
    if (hint[0])
    {
        ButtonGlyphs::drawInlineHint(badgeDisplay, 0, kMpyFooterBaseY, hint);
    }

    makeActionHint(hint, sizeof(hint), right_button, right_label);
    if (hint[0])
    {
        ButtonGlyphs::drawInlineHintRight(
            badgeDisplay, OLEDLayout::kScreenW, kMpyFooterBaseY, hint);
    }
}

}  // namespace

extern "C" int temporalbadge_runtime_ui_header(const char *title,
                                                const char *right)
{
    mpy_oled_note_activity();
    prepareUiDraw();
    OLEDLayout::drawHeader(badgeDisplay, title, right);
    return 1;
}

extern "C" int temporalbadge_runtime_ui_action_bar(
    const char *left_button, const char *left_label,
    const char *right_button, const char *right_label)
{
    mpy_oled_note_activity();
    drawActionBar(left_button, left_label, right_button, right_label);
    return 1;
}

extern "C" int temporalbadge_runtime_ui_chrome(
    const char *title, const char *right, const char *left_button,
    const char *left_label, const char *right_button, const char *right_label)
{
    mpy_oled_note_activity();
    badgeDisplay.clearDisplay();
    prepareUiDraw();
    OLEDLayout::drawHeader(badgeDisplay, title, right);
    drawActionBar(left_button, left_label, right_button, right_label);
    return 1;
}

extern "C" int temporalbadge_runtime_ui_inline_hint(int x, int y,
                                                    const char *hint)
{
    if (!hint)
    {
        return 0;
    }
    mpy_oled_note_activity();
    prepareUiDraw();
    return ButtonGlyphs::drawInlineHint(badgeDisplay, x, y + 9, hint);
}

extern "C" int temporalbadge_runtime_ui_inline_hint_right(int right_x, int y,
                                                          const char *hint)
{
    if (!hint)
    {
        return 0;
    }
    mpy_oled_note_activity();
    prepareUiDraw();
    return ButtonGlyphs::drawInlineHintRight(badgeDisplay, right_x, y + 9,
                                             hint);
}

extern "C" int temporalbadge_runtime_ui_measure_hint(const char *hint)
{
    if (!hint)
    {
        return 0;
    }
    prepareUiDraw();
    return ButtonGlyphs::measureInlineHint(badgeDisplay, hint);
}

// ── Native-grid menu primitives ─────────────────────────────────────────────
// Lets MicroPython render a 2x2 menu using the exact same cell geometry as
// the home GridMenuScreen, so apps like the IR Playground feel native.

namespace {

struct IconRegistryEntry
{
    const char *name;
    const uint8_t *data;
    uint8_t w;
    uint8_t h;
};

// Small whitelist of icon names. Keep this short — adding an entry here
// is the only way Python can reference a native icon, so the surface
// stays tractable. Names are matched case-insensitively but tokens
// should stay lowercase by convention.
static const IconRegistryEntry kIconRegistry[] = {
    {"apps",       AppIcons::apps,           AppIcons::kW, AppIcons::kH},
    {"booper",     AppIcons::booper,         AppIcons::kW, AppIcons::kH},
    {"games",      AppIcons::games,          AppIcons::kW, AppIcons::kH},
    {"profile",    AppIcons::profile,        AppIcons::kW, AppIcons::kH},
    {"settings",   AppIcons::settings,       AppIcons::kW, AppIcons::kH},
    {"wifi",       AppIcons::wifi,           AppIcons::kW, AppIcons::kH},
    {"map",        AppIcons::map,            AppIcons::kW, AppIcons::kH},
    {"docs",       AppIcons::docs,           AppIcons::kW, AppIcons::kH},
    {"schedule",   AppIcons::schedule,       AppIcons::kW, AppIcons::kH},
    {"directory",  AppIcons::directory,      AppIcons::kW, AppIcons::kH},
    {"synth",      AppIcons::synth,          AppIcons::kW, AppIcons::kH},
    {"ir_play",    AppIcons::irPlayground,   AppIcons::kW, AppIcons::kH},
    {"ir_block",   AppIcons::irBlockBattle,  AppIcons::kW, AppIcons::kH},
};

bool resolveIcon(const char *name, const uint8_t **out, uint8_t *w, uint8_t *h)
{
    if (out) *out = nullptr;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!name || !*name) return false;
    for (const auto &entry : kIconRegistry)
    {
        if (strcmp(entry.name, name) == 0)
        {
            if (out) *out = entry.data;
            if (w) *w = entry.w;
            if (h) *h = entry.h;
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" int temporalbadge_runtime_ui_grid_cell(int col, int row,
                                                   const char *label,
                                                   int selected,
                                                   const char *icon_name)
{
    if (col < 0 || col > 1 || row < 0 || row > 1) return 0;
    mpy_oled_note_activity();
    prepareUiDraw();

    const uint8_t *iconData = nullptr;
    uint8_t iconW = 0;
    uint8_t iconH = 0;
    if (icon_name && *icon_name)
    {
        resolveIcon(icon_name, &iconData, &iconW, &iconH);
    }
    OLEDLayout::drawGridCell(badgeDisplay, (uint8_t)col, (uint8_t)row,
                              label ? label : "",
                              selected != 0,
                              iconData, iconW, iconH);
    return 1;
}

extern "C" int temporalbadge_runtime_ui_grid_footer(const char *description)
{
    mpy_oled_note_activity();
    prepareUiDraw();
    OLEDLayout::drawGridFooter(badgeDisplay, description ? description : "");
    return 1;
}

// ── Native list-menu row primitive ──────────────────────────────────────────
// Renders a single list row using the same geometry every native list
// screen uses (Settings, Wifi, MenuOrder, etc.):
//   kContentY = 10, kRowHeight = 11, UIFonts::kText (Smallsimple 5x7),
//   text baseline = y + getAscent() + 1, drawSelectedRow for highlight,
//   text colour inverted on the selected row.
//
// Caller is responsible for clearing the screen, drawing chrome, and
// computing which slice of the items array is visible (the standard
// IR Playground top bar leaves room for 4 rows: indices 0..3).

extern "C" int temporalbadge_runtime_ui_list_row(int row,
                                                  const char *label,
                                                  int selected)
{
    if (row < 0) return 0;
    mpy_oled_note_activity();
    prepareUiDraw();

    constexpr int kRowH = 11;
    constexpr int kContentY = 10;
    const int y = kContentY + row * kRowH;
    if (y + kRowH > OLEDLayout::kFooterTopY) return 0;

    badgeDisplay.setFont(UIFonts::kText);

    if (selected != 0)
    {
        OLEDLayout::drawSelectedRow(badgeDisplay, y, kRowH,
                                     /*x=*/0, /*w=*/OLEDLayout::kScreenW);
        badgeDisplay.setDrawColor(0);
    }
    else
    {
        badgeDisplay.setDrawColor(1);
    }

    if (label && *label)
    {
        char buf[40] = {};
        strncpy(buf, label, sizeof(buf) - 1);
        OLEDLayout::fitText(badgeDisplay, buf, sizeof(buf),
                             OLEDLayout::kScreenW - 6);
        const int baseY = y + badgeDisplay.getAscent() + 1;
        badgeDisplay.drawStr(3, baseY, buf);
    }
    badgeDisplay.setDrawColor(1);
    return 1;
}

// Returns how many rows fit in the body region — exposed so MP-side
// list_menu can paginate without hard-coding the cell height.
extern "C" int temporalbadge_runtime_ui_list_rows_visible(void)
{
    constexpr int kRowH = 11;
    constexpr int kContentY = 10;
    const int avail = OLEDLayout::kFooterTopY - kContentY;
    return avail > 0 ? (avail / kRowH) : 0;
}
