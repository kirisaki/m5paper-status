#pragma once

#include <stdint.h>
#include <string.h>

#include "build_config.h"

namespace config {
constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kWifiRetryMs = 30000;

inline const char* apiToken(const char* name) {
  for (const auto& token : kApiTokens) {
    if (token.name != nullptr && strcmp(token.name, name) == 0) {
      return token.value;
    }
  }
  return "";
}
}  // namespace config
