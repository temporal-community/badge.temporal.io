// Apps-related MicroPython bindings (currently just rescan_apps()).
//
// JumperIDE and other live-coding workflows write a script to
// /apps/<slug>/main.py and then call badge.rescan_apps() to make the
// new app appear on the main-menu grid without rebooting. The actual
// scan walks the FAT VFS, parses the dunder metadata, and rebuilds
// the menu vector — see firmware/src/apps/AppRegistry.cpp and
// firmware/src/ui/GUI.cpp.

#include <stddef.h>

#include "../../apps/AppRegistry.h"

extern "C" {
#include "temporalbadge_runtime.h"
void rebuildMainMenuFromRegistry(void);
}

extern "C" int temporalbadge_runtime_rescan_apps(void) {
  rebuildMainMenuFromRegistry();
  return static_cast<int>(AppRegistry::count());
}
