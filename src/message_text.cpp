#include "message_text.h"

#include <cstdint>

namespace {
bool decode(const std::string& text, size_t& pos, uint32_t& code) {
  const auto first = static_cast<uint8_t>(text[pos++]);
  if (first < 0x80) {
    code = first;
    return true;
  }
  unsigned remaining;
  uint32_t minimum;
  if (first >= 0xC2 && first <= 0xDF) {
    remaining = 1; minimum = 0x80; code = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    remaining = 2; minimum = 0x800; code = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    remaining = 3; minimum = 0x10000; code = first & 0x07;
  } else {
    return false;
  }
  if (text.size() - pos < remaining) return false;
  while (remaining--) {
    const auto next = static_cast<uint8_t>(text[pos++]);
    if ((next & 0xC0) != 0x80) return false;
    code = (code << 6) | (next & 0x3F);
  }
  return code >= minimum && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF);
}

void popCharacter(std::string& text) {
  size_t pos = text.size() - 1;
  while (pos && (static_cast<uint8_t>(text[pos]) & 0xC0) == 0x80) --pos;
  text.resize(pos);
}
}  // namespace

namespace message_text {
bool valid(const std::string& text) {
  size_t pos = 0;
  while (pos < text.size()) {
    uint32_t code;
    if (!decode(text, pos, code)) return false;
    if ((code < 32 && code != '\n' && code != '\r' && code != '\t') ||
        (code >= 0x7F && code <= 0x9F)) return false;
  }
  return true;
}

Layout wrap(const std::string& text, int width, size_t maxLines, const Measure& measure) {
  Layout result;
  if (text.empty()) return result;
  if (maxLines == 0 || width <= 0 || !valid(text)) {
    result.truncated = true;
    return result;
  }
  result.lines.emplace_back();
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t start = pos;
    uint32_t code;
    decode(text, pos, code);
    if (code == '\r' || code == '\n') {
      if (code == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
      if (result.lines.size() == maxLines) {
        result.truncated = true;
        break;
      }
      result.lines.emplace_back();
      continue;
    }
    // OpenFontRender's decoder is limited to the BMP; keep the original
    // message intact in storage and replace supplementary characters only on screen.
    const std::string glyph = code > 0xFFFF ? "□" :
                              code == '\t' ? "    " : text.substr(start, pos - start);
    std::string candidate = result.lines.back() + glyph;
    // Bound layout work even for thousands of combining or zero-width glyphs.
    if (candidate.size() > 240 || measure(candidate) > width) {
      if (result.lines.size() == maxLines || measure(glyph) > width) {
        result.truncated = true;
        break;
      }
      result.lines.emplace_back(glyph);
    } else {
      result.lines.back() = candidate;
    }
  }
  if (result.truncated) {
    auto& last = result.lines.back();
    while (!last.empty() && measure(last + "…") > width) popCharacter(last);
    if (measure(last + "…") <= width) last += "…";
  }
  return result;
}
Page wrapPage(const std::string& text, int width, size_t firstLine, size_t maxLines, const Measure& measure) {
  Page result;
  if (text.empty() || width <= 0 || !maxLines || !valid(text)) return result;
  // Scan all line boundaries but retain only the visible page: even 4096
  // newlines must not allocate thousands of strings on the ESP32 heap.
  const auto emit = [&](const std::string& line) {
    if (result.totalLines >= firstLine && result.lines.size() < maxLines) result.lines.push_back(line);
    ++result.totalLines;
  };
  std::string line;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t start = pos;
    uint32_t code;
    decode(text, pos, code);
    if (code == '\r' || code == '\n') {
      if (code == '\r' && pos < text.size() && text[pos] == '\n') ++pos;
      emit(line); line.clear(); continue;
    }
    const std::string glyph = code > 0xFFFF ? "□" : code == '\t' ? "    " : text.substr(start, pos - start);
    const std::string candidate = line + glyph;
    if (!line.empty() && (candidate.size() > 240 || measure(candidate) > width)) {
      emit(line); line.clear();
    }
    line += glyph;
  }
  emit(line);
  return result;
}
}  // namespace message_text
