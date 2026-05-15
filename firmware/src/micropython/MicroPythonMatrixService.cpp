#include "MicroPythonMatrixService.h"

#include <Arduino.h>
#include <cstdio>

#include "../led/LEDAppRuntime.h"

extern "C" {
#include "matrix_app_api.h"
void mpy_gui_exec_file(const char* path);
}

void MicroPythonMatrixService::service() {
  // Drain a pending Python matrix-app registration request before ticking.
  // restoreAmbient() (called from boot and after commitMatrixApp) sets this
  // when the persisted state names a slug. We exec the slug's matrix.py once;
  // it registers a callback via badge.matrix_app_start(...) that this service
  // ticks forever after.
  char slug[LEDAppRuntime::kPythonSlugCap];
  if (ledAppRuntime.consumePendingExec(slug, sizeof(slug))) {
    char path[64];
    std::snprintf(path, sizeof(path), "/apps/%s/matrix.py", slug);
    Serial.printf("[matrix] sourcing persistent matrix app: %s\n", path);
    mpy_gui_exec_file(path);
  }
  matrix_app_service_tick(millis());
}

const char* MicroPythonMatrixService::name() const {
  return "MPMatrixApp";
}

void MicroPythonMatrixService::beginOverride() {
  matrix_app_begin_override();
}

void MicroPythonMatrixService::endOverride() {
  matrix_app_end_override();
}

bool MicroPythonMatrixService::isOverridden() const {
  return matrix_app_is_overridden() != 0;
}

bool MicroPythonMatrixService::hasCallback() const {
  return matrix_app_is_active() != 0;
}
