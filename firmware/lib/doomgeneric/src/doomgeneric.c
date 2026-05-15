#include <stdio.h>
#include <stdlib.h>

#include "m_argv.h"

#include "doomgeneric.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

#ifdef ESP_PLATFORM
	if (!DG_ScreenBuffer) {
		DG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4, MALLOC_CAP_SPIRAM);
		if (DG_ScreenBuffer == NULL) {
			DG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4, MALLOC_CAP_8BIT);
		}
	}
#else
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
#endif

	DG_Init();

	D_DoomMain ();
}
