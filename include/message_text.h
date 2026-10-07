#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace message_text {
constexpr size_t kMaxBytes = 4096;
bool valid(const std::string& text);
struct Layout {
  std::vector<std::string> lines;
  bool truncated = false;
};
using Measure = std::function<int(const std::string&)>;
Layout wrap(const std::string& text, int width, size_t maxLines, const Measure& measure);
}  // namespace message_text
