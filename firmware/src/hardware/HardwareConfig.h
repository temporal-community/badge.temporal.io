#pragma once

// Hardware target selector for the public Replay 2026 Badge firmware.
// Set via build_flags in platformio.ini: -DHARDWARE_ECHO

#if defined(HARDWARE_ECHO)
  #include "EchoDefines.h"
#else
  #error "No hardware target defined. Add -DHARDWARE_ECHO to platformio.ini build_flags."
#endif
