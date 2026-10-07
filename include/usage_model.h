#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usage {
struct Window {
  bool available = false;
  double usedPercent = 0;
  int64_t resetsAt = 0;  // 0 means no reset time was supplied.
};
struct Provider {
  bool configured = false;
  int64_t fetchedAt = 0;
  Window session;
  Window weekly;
  struct ModelLimit { std::string name; Window window; };
  std::vector<ModelLimit> models;
  std::string error;
};
struct Snapshot { Provider codex; Provider claude; };
bool expired(const Window& window, int64_t now);
bool stale(const Provider& provider, int64_t now);
std::string resetLabel(const Provider& provider, const Window& window, int64_t now);
int fillWidth(const Window& window, int width);
}  // namespace usage
