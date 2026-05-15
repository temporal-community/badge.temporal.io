#ifdef BADGE_HAS_DOOM 

#include "doom_app.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <setjmp.h>
#include <cstring>

extern "C" {
#include "doomgeneric.h"
#ifdef ESP_PLATFORM
#include "doom_esp32_mem.h"
void I_ResetForRelaunch(void);
#endif
extern jmp_buf doom_exit_jmp;
extern volatile int doom_exit_code;
}

void doom_hal_deinit(void);

#include "doom_render.h"
#include "doom_input.h"
#include "doom_hud_matrix.h"

static doom_app_config_t s_cfg;
static volatile bool s_running = false;
static volatile bool s_stop_requested = false;
static TaskHandle_t s_doom_task = NULL;

static constexpr uint32_t kDoomTaskStackBytes = 24 * 1024;
static constexpr UBaseType_t kDoomTaskPrio = 5;
static constexpr BaseType_t kDoomCore = 1;

static StaticTask_t s_doom_task_tcb;
static StackType_t* s_doom_task_stack = nullptr;
static bool s_stack_alloc_attempted = false;

const doom_app_config_t* doom_app_get_config(void) {
    return &s_cfg;
}

bool doom_app_stop_requested(void) {
    return s_stop_requested;
}

static void doom_task_fn(void* param) {
    (void)param;
    Serial.println("[doom] task started");

#ifdef ESP_PLATFORM
    I_ResetForRelaunch();
#endif

    if (setjmp(doom_exit_jmp) == 0) {
        char arg0[] = "doom";
        char arg_iwad[] = "-iwad";
        char wad_path[128];
        strncpy(wad_path, s_cfg.wad_path ? s_cfg.wad_path : "/doom1.wad",
                sizeof(wad_path) - 1);
        wad_path[sizeof(wad_path) - 1] = '\0';

        char* argv[] = { arg0, arg_iwad, wad_path, NULL };
        int argc = 3;

        doomgeneric_Create(argc, argv);

        while (!s_stop_requested) {
            doomgeneric_Tick();
        }
    } else {
        Serial.printf("[doom] engine exited (code %d)\n", doom_exit_code);
    }

    s_running = false;
    Serial.printf("[doom] task stack_hwm_bytes=%u\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    Serial.println("[doom] task ending");
    TaskHandle_t self = s_doom_task;
    s_doom_task = NULL;
    vTaskDelete(self);
}

doom_app_status_t doom_app_init(const doom_app_config_t* cfg) {
    if (!cfg) return DOOM_APP_ERR;
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_stop_requested = false;
    s_running = false;

    if (!s_stack_alloc_attempted) {
        s_stack_alloc_attempted = true;
        s_doom_task_stack = (StackType_t*)heap_caps_malloc(
            kDoomTaskStackBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_doom_task_stack) {
            Serial.printf("[doom] pre-allocated %u byte task stack from internal SRAM\n",
                          (unsigned)kDoomTaskStackBytes);
        } else {
            Serial.println("[doom] WARNING: failed to pre-allocate task stack");
        }
    }

#ifdef ESP_PLATFORM
    doom_esp32_mem_init();
#endif
    doom_render_init();
    doom_input_init();
    doom_hud_init();
    Serial.println("[doom] initialized");
    return DOOM_APP_OK;
}

doom_app_status_t doom_app_enter(void) {
    if (s_running) return DOOM_APP_ERR;

    s_stop_requested = false;
    s_running = true;

    if (s_doom_task_stack) {
        s_doom_task = xTaskCreateStaticPinnedToCore(
            doom_task_fn, "doom", kDoomTaskStackBytes, NULL,
            kDoomTaskPrio, s_doom_task_stack, &s_doom_task_tcb, kDoomCore);
    } else {
        BaseType_t ret = xTaskCreatePinnedToCore(
            doom_task_fn, "doom", kDoomTaskStackBytes, NULL,
            kDoomTaskPrio, &s_doom_task, kDoomCore);
        if (ret != pdPASS) s_doom_task = NULL;
    }

    if (s_doom_task == NULL) {
        s_running = false;
        Serial.println("[doom] failed to create task");
        return DOOM_APP_ERR;
    }

    Serial.println("[doom] entered");
    return DOOM_APP_OK;
}

doom_app_status_t doom_app_exit(void) {
    if (!s_running && s_doom_task == NULL) return DOOM_APP_OK;

    s_stop_requested = true;

    uint32_t timeout = millis() + 3000;
    while (s_doom_task != NULL && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_doom_task != NULL) {
        Serial.printf("[doom] forced delete stack_hwm_bytes=%u\n",
                      (unsigned)uxTaskGetStackHighWaterMark(s_doom_task));
        Serial.println("[doom] task didn't exit cleanly, deleting");
        vTaskDelete(s_doom_task);
        s_doom_task = NULL;
    }

    s_running = false;
    doom_render_deinit();
    doom_hud_deinit();
#ifdef ESP_PLATFORM
    doom_esp32_mem_deinit();
#endif
    doom_hal_deinit();

    Serial.println("[doom] exited");
    return DOOM_APP_OK;
}

doom_app_status_t doom_app_deinit(void) {
    doom_app_exit();
    if (s_doom_task_stack) {
        heap_caps_free(s_doom_task_stack);
        s_doom_task_stack = nullptr;
        s_stack_alloc_attempted = false;
        Serial.printf("[doom] released %u byte task stack\n",
                      (unsigned)kDoomTaskStackBytes);
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    Serial.println("[doom] deinitialized");
    return DOOM_APP_OK;
}

bool doom_app_is_running(void) {
    return s_running;
}

void doom_app_request_stop(void) {
    s_stop_requested = true;
}

#endif // BADGE_HAS_DOOM
