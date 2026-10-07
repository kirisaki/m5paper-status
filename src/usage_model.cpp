#include "usage_model.h"
#include "calendar_model.h"
#include <algorithm>
#include <cmath>

namespace usage {
bool expired(const Window& window, int64_t now) {
  return window.available && window.resetsAt > 0 && now >= window.resetsAt;
}
bool stale(const Provider& provider, int64_t now) {
  return provider.fetchedAt > 0 && (now - provider.fetchedAt > 900 || !provider.error.empty());
}
std::string resetLabel(const Provider& provider, const Window& window, int64_t now) {
  if (!provider.configured) return "未設定";
  if (!provider.fetchedAt) return provider.error.empty() ? "取得中" : "取得失敗";
  if (expired(window, now)) return "更新待ち";
  if (!window.available) return "対象枠なし";
  if (!window.resetsAt) return stale(provider, now) ? "前回値・時刻未定" : "リセット未定";
  const auto stamp = calendar::rfc3339(window.resetsAt);
  return (stale(provider, now) ? "前回 " : "リセット ") + stamp.substr(5, 2) + "/" + stamp.substr(8, 2) + " " + stamp.substr(11, 5);
}
int fillWidth(const Window& window, int width) {
  if (!window.available || width <= 0 || !std::isfinite(window.usedPercent)) return 0;
  return static_cast<int>(std::lround(std::max(0.0, std::min(100.0, window.usedPercent)) * width / 100.0));
}
}  // namespace usage
