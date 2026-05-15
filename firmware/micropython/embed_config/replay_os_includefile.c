// Included into extmod/modos.c via MICROPY_PY_OS_INCLUDEFILE.
// Upstream ports/esp32/modos.c calls esp_random() with only esp_system.h; Arduino-ESP32 3.x
// exposes the prototype in esp_random.h.
#include "esp_random.h"
#include "ports/esp32/modos.c"
