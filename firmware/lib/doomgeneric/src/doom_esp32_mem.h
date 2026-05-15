#ifndef DOOM_ESP32_MEM_H
#define DOOM_ESP32_MEM_H

#ifdef ESP_PLATFORM

#ifdef __cplusplus
extern "C" {
#endif

void doom_esp32_mem_init(void);
void doom_esp32_mem_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // ESP_PLATFORM
#endif // DOOM_ESP32_MEM_H
