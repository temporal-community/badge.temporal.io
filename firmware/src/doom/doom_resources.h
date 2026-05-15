#ifndef DOOM_RESOURCES_H
#define DOOM_RESOURCES_H

#ifdef BADGE_HAS_DOOM

void doom_resources_enter(void);
void doom_resources_exit(void);
bool doom_resources_active(void);

void doom_resources_pause_for_keyboard(void);
void doom_resources_resume_from_keyboard(void);
bool doom_resources_keyboard_active(void);

#endif  // BADGE_HAS_DOOM
#endif  // DOOM_RESOURCES_H
