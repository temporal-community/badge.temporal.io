#pragma once

#if defined(__has_include)
#  if __has_include("BuildWifiConfig.generated.h")
#    include "BuildWifiConfig.generated.h"
#    define BADGE_HAS_GENERATED_WIFI_CONFIG 1
#  endif
#endif

#ifndef BADGE_HAS_GENERATED_WIFI_CONFIG
#include <stddef.h>

namespace BuildWifiConfig {
inline bool hasWifi() {
  return false;
}

inline void decodeSsid(char* out, size_t cap) {
  if (out != nullptr && cap > 0) out[0] = '\0';
}

inline void decodePass(char* out, size_t cap) {
  if (out != nullptr && cap > 0) out[0] = '\0';
}
}  // namespace BuildWifiConfig
#endif
