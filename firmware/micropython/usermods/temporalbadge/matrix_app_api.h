#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int matrix_app_service_tick(uint32_t now_ms);
void matrix_app_foreground_begin(void);
void matrix_app_foreground_end(void);
void matrix_app_begin_override(void);
void matrix_app_end_override(void);
int matrix_app_is_overridden(void);
int matrix_app_is_active(void);
void matrix_app_stop_from_c(void);
uint32_t matrix_app_interval_ms(void);

#ifdef __cplusplus
}
#endif
