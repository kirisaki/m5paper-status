#include "usage_model.h"
#include "calendar_model.h"
#include <cassert>
#include <limits>

int main() {
  usage::Provider provider;
  usage::Window window;
  int64_t now, reset;
  assert(calendar::parseDateTime("2026-10-07T18:00:00+09:00", now));
  assert(calendar::parseDateTime("2026-10-13T23:59:59.5+00:00", reset));
  assert(usage::resetLabel(provider, window, now) == "未設定");
  provider.configured = true;
  assert(usage::resetLabel(provider, window, now) == "取得中");
  provider.error = "auth_http_401";
  assert(usage::resetLabel(provider, window, now) == "取得失敗");
  provider.error.clear(); provider.fetchedAt = now;
  assert(usage::resetLabel(provider, window, now) == "対象枠なし");
  window.available = true; window.usedPercent = 0;
  assert(usage::resetLabel(provider, window, now) == "リセット未定");
  assert(usage::fillWidth(window, 100) == 0);
  window.usedPercent = 25.5;
  assert(usage::fillWidth(window, 200) == 51);
  window.usedPercent = 120;
  assert(usage::fillWidth(window, 200) == 200);
  window.usedPercent = std::numeric_limits<double>::quiet_NaN();
  assert(usage::fillWidth(window, 200) == 0);
  window.resetsAt = reset;
  assert(usage::resetLabel(provider, window, now) == "リセット 10/14 08:59");
  assert(!usage::expired(window, reset - 1));
  assert(usage::expired(window, reset));
  assert(usage::resetLabel(provider, window, reset) == "更新待ち");
  assert(usage::stale(provider, now + 901));
  assert(usage::resetLabel(provider, window, now + 901) == "前回 10/14 08:59");
  provider.error = "usage_http_503";
  assert(usage::stale(provider, now));
}
