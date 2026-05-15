#include <string.h>

#include "../../hardware/LEDmatrix.h"

#include "Internal.h"
#include "temporalbadge_runtime.h"

extern LEDmatrix badgeMatrix;

extern "C" int temporalbadge_runtime_led_set_brightness(int brightness)
{
    if (brightness < 0 || brightness > 255)
    {
        return -1;
    }
    mpy_led_note_activity();
    badgeMatrix.setBrightness((uint8_t)brightness);
    return 0;
}

extern "C" int temporalbadge_runtime_led_clear(void)
{
    mpy_led_note_activity();
    return badgeMatrix.clear() ? 1 : 0;
}

extern "C" int temporalbadge_runtime_led_fill(int brightness)
{
    mpy_led_note_activity();
    if (brightness < 0)
    {
        return badgeMatrix.fill() ? 1 : 0;
    }
    return badgeMatrix.fill((uint8_t)brightness) ? 1 : 0;
}

extern "C" int temporalbadge_runtime_led_set_pixel(int x, int y, int brightness)
{
    if (x < 0 || x > 7 || y < 0 || y > 7)
        return -1;
    mpy_led_note_activity();
    return badgeMatrix.setPixel((uint8_t)x, (uint8_t)y, (uint8_t)brightness) ? 1 : 0;
}

extern "C" int temporalbadge_runtime_led_get_pixel(int x, int y)
{
    if (x < 0 || x > 7 || y < 0 || y > 7)
        return -1;
    return badgeMatrix.getPixel((uint8_t)x, (uint8_t)y);
}

extern "C" int temporalbadge_runtime_led_show_image(const char *name)
{
    if (!name)
        return -1;
    mpy_led_note_activity();
    return badgeMatrix.showImageById(name) ? 1 : 0;
}

extern "C" int temporalbadge_runtime_led_set_frame(const uint8_t *rows, int brightness)
{
    if (!rows)
        return -1;
    mpy_led_note_activity();
    uint8_t b = (brightness < 0)
                    ? badgeMatrix.getBrightness()
                    : (uint8_t)brightness;
    badgeMatrix.drawMaskHardware(rows, b, 0);
    return 1;
}

extern "C" int temporalbadge_runtime_led_start_animation(const char *name, int interval_ms)
{
    if (!name)
        return -1;
    mpy_led_note_activity();
    uint16_t ival = (interval_ms > 0) ? (uint16_t)interval_ms : 120;

    if (strcmp(name, "spinner") == 0)
    {
        return badgeMatrix.startAnimation(
                   LEDmatrix::DefaultAnimation::Spinner, ival)
                   ? 1
                   : 0;
    }
    if (strcmp(name, "blink_smiley") == 0)
    {
        return badgeMatrix.startAnimation(
                   LEDmatrix::DefaultAnimation::BlinkSmiley, ival)
                   ? 1
                   : 0;
    }
    if (strcmp(name, "pulse_heart") == 0)
    {
        return badgeMatrix.startAnimation(
                   LEDmatrix::DefaultAnimation::PulseHeart, ival)
                   ? 1
                   : 0;
    }
    return 0;
}

extern "C" int temporalbadge_runtime_led_stop_animation(void)
{
    mpy_led_note_activity();
    return badgeMatrix.stopAnimation() ? 1 : 0;
}

extern "C" int temporalbadge_runtime_matrix_host_apply_brightness(int brightness)
{
    if (brightness < 0)
    {
        brightness = 0;
    }
    else if (brightness > 255)
    {
        brightness = 255;
    }
    badgeMatrix.setBrightness((uint8_t)brightness);
    return brightness;
}

extern "C" void temporalbadge_runtime_matrix_host_set_active(int active)
{
    if (active)
    {
        if (!mpy_led_hold)
        {
            mpy_led_hold = true;
        }
        badgeMatrix.setMicropythonMode(true);
    }
    else
    {
        if (mpy_led_hold)
        {
            mpy_led_hold = false;
        }
        badgeMatrix.setMicropythonMode(false);
    }
}

extern "C" int temporalbadge_runtime_matrix_default_interval_ms(void)
{
    return 150;
}

extern "C" int temporalbadge_runtime_matrix_default_brightness(void)
{
    return badgeMatrix.getBrightness();
}
