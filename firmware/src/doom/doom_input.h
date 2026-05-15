#ifndef DOOM_INPUT_H
#define DOOM_INPUT_H

#ifdef BADGE_HAS_DOOM

#include <stdint.h>
#include <stdbool.h>

// Button bitmask bits from get_input callback
#define DOOM_BTN_UP    (1 << 0)
#define DOOM_BTN_DOWN  (1 << 1)
#define DOOM_BTN_LEFT  (1 << 2)
#define DOOM_BTN_RIGHT (1 << 3)

#ifdef __cplusplus
extern "C" {
#endif

void doom_input_init(void);

// Poll hardware and fill the DoomGeneric key queue.
// Returns true if exit combo has been held long enough.
bool doom_input_poll(void);

// Called by DG_GetKey to dequeue pressed/released events.
int doom_input_get_key(int* pressed, unsigned char* key);

#ifdef __cplusplus
}
#endif

#endif // BADGE_HAS_DOOM
#endif // DOOM_INPUT_H
