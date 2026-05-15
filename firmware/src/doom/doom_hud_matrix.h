#ifndef DOOM_HUD_MATRIX_H
#define DOOM_HUD_MATRIX_H

#ifdef BADGE_HAS_DOOM

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void doom_hud_init(void);
void doom_hud_deinit(void);

// Update the 8x8 LED matrix HUD.
// health/armor/ammo 0-100, keys bitmask (b0=red, b1=blue, b2=yellow).
void doom_hud_update(uint8_t health, uint8_t armor, uint8_t ammo, uint8_t keys);

// Set a message to scroll across the matrix (full 8px height).
// String is copied internally. Pass NULL to cancel.
void doom_hud_set_message(const char* msg);

#ifdef __cplusplus
}
#endif

#endif // BADGE_HAS_DOOM
#endif // DOOM_HUD_MATRIX_H
