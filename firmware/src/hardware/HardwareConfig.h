#pragma once

// Hardware target selector.
// Set via build_flags in platformio.ini: -DHARDWARE_DELTA or -DHARDWARE_CHARLIE

#if defined(HARDWARE_DELTA)
  #include "DeltaDefines.h"
#elif defined(HARDWARE_CHARLIE)
  #include "CharlieDefines.h"
#elif defined(HARDWARE_ECHO)
  #include "EchoDefines.h"
#else
  #error "No hardware target defined. Add -DHARDWARE_DELTA, -DHARDWARE_CHARLIE, or -DHARDWARE_ECHO to platformio.ini build_flags."
#endif
