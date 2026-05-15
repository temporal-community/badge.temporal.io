#ifdef ESP_PLATFORM

#include "doom_esp32_mem.h"
#include "esp_heap_caps.h"
#include "r_defs.h"
#include "r_plane.h"
#include "r_main.h"
#include "r_bsp.h"
#include "r_things.h"
#include "p_local.h"
#include "tables.h"
#include "i_video.h"
#include "info.h"
#include "d_loop.h"
#include <string.h>
#include <stdio.h>

extern visplane_t* visplanes;
extern short*      openings;
extern int*        viewangletox;
extern drawseg_t*  drawsegs;

// Flash-resident initializer tables (declared static const in info.c)
extern const state_t    states_init[];
extern const mobjinfo_t mobjinfo_init[];

#ifndef MAXVISPLANES
#define MAXVISPLANES 64
#endif

#define MAXOPENINGS (SCREENWIDTH * 64)

static void* psram_calloc(size_t count, size_t size) {
    void* p = heap_caps_malloc(count * size, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, count * size);
    return p;
}

void doom_esp32_mem_init(void) {
    if (!visplanes)
        visplanes = psram_calloc(MAXVISPLANES, sizeof(visplane_t));
    if (!openings)
        openings = psram_calloc(MAXOPENINGS, sizeof(short));
    if (!viewangletox)
        viewangletox = psram_calloc(FINEANGLES / 2, sizeof(int));
    if (!drawsegs)
        drawsegs = psram_calloc(MAXDRAWSEGS, sizeof(drawseg_t));

    if (!states) {
        states = heap_caps_malloc(NUMSTATES * sizeof(state_t), MALLOC_CAP_SPIRAM);
        if (states) memcpy(states, states_init, NUMSTATES * sizeof(state_t));
    }
    if (!mobjinfo) {
        mobjinfo = heap_caps_malloc(NUMMOBJTYPES * sizeof(mobjinfo_t), MALLOC_CAP_SPIRAM);
        if (mobjinfo) memcpy(mobjinfo, mobjinfo_init, NUMMOBJTYPES * sizeof(mobjinfo_t));
    }

    if (!zlight)
        zlight = psram_calloc(LIGHTLEVELS, sizeof(*zlight));
    if (!scalelight)
        scalelight = psram_calloc(LIGHTLEVELS, sizeof(*scalelight));
    if (!vissprites)
        vissprites = psram_calloc(MAXVISSPRITES, sizeof(vissprite_t));
    if (!intercepts)
        intercepts = psram_calloc(MAXINTERCEPTS, sizeof(intercept_t));
    if (!ticdata)
        ticdata = psram_calloc(BACKUPTICS, sizeof(ticcmd_set_t));

    printf("[doom_mem] PSRAM: visplanes=%p openings=%p viewangletox=%p drawsegs=%p states=%p mobjinfo=%p\n",
           visplanes, openings, viewangletox, drawsegs, states, mobjinfo);
    printf("[doom_mem] PSRAM: zlight=%p scalelight=%p vissprites=%p intercepts=%p\n",
           zlight, scalelight, vissprites, intercepts);
}

void doom_esp32_mem_deinit(void) {
    if (visplanes)    { heap_caps_free(visplanes);    visplanes = NULL; }
    if (openings)     { heap_caps_free(openings);     openings = NULL; }
    if (viewangletox) { heap_caps_free(viewangletox); viewangletox = NULL; }
    if (drawsegs)     { heap_caps_free(drawsegs);     drawsegs = NULL; }
    if (states)       { heap_caps_free(states);       states = NULL; }
    if (mobjinfo)     { heap_caps_free(mobjinfo);     mobjinfo = NULL; }

    // zlight, scalelight, vissprites, intercepts, ticdata are kept
    // allocated across relaunches — Doom globals cache pointers into
    // them, and the relaunch reset path doesn't clear every one.
    // Permanent PSRAM cost is ~18 KB (negligible vs 8 MB).
}

#endif // ESP_PLATFORM
