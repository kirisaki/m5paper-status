#include "message_text.h"

#include <cassert>
#include <iostream>

// Widths chosen to exercise mixed full-width Japanese and narrow Latin letters.
int measure(const std::string& text) {
  int width = 0;
  for (unsigned char c : text) {
    if (c < 0x80) width += 1;
    else if ((c & 0xC0) != 0x80) width += 2;
  }
  return width;
}

int main() {
  using message_text::valid;
  using message_text::wrap;
  assert(valid("日本語 ABC\n次の行\r\n\t😊"));
  for (const std::string& text : {std::string("a\0b", 3), std::string("\x01", 1),
       std::string("\x7f", 1), std::string("\xc2\x80", 2), std::string("\x80", 1),
       std::string("\xc0\xaf", 2), std::string("\xe0\x80\x80", 3),
       std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
       std::string("\xe3\x81", 2), std::string("\xe3\x41\x81", 3)}) {
    assert(!valid(text));
  }
  const auto japanese = wrap("あいうえお", 4, 3, measure);
  assert((japanese.lines == std::vector<std::string>{"あい", "うえ", "お"}));
  assert(!japanese.truncated);
  const auto clipped = wrap("あいうえお", 4, 2, measure);
  assert((clipped.lines == std::vector<std::string>{"あい", "う…"}));
  assert(clipped.truncated);
  const auto exact = wrap("あいうえ", 4, 2, measure);
  assert(!exact.truncated);
  const auto newline = wrap("a\r\nb\rc\n\n\td", 20, 8, measure);
  assert((newline.lines == std::vector<std::string>{"a", "b", "c", "", "    d"}));
  const auto symbol = wrap("a😊b", 20, 1, measure);
  assert(symbol.lines[0] == "a□b");
  assert(!symbol.truncated);
  assert(wrap("", 20, 3, measure).lines.empty());
  assert(wrap("a", 20, 0, measure).truncated);
  assert(wrap("a", 0, 1, measure).truncated);
  assert(wrap("a\nb", 20, 1, measure).lines[0] == "a…");
  const auto longWord = wrap(std::string(4096, 'a'), 30, 7, measure);
  assert(longWord.truncated && longWord.lines.size() == 7);
  for (const auto& line : longWord.lines) assert(valid(line) && measure(line) <= 30);
  const auto zeroWidth = wrap(std::string(4096, 'a'), 30, 7,
                              [](const std::string&) { return 0; });
  assert(zeroWidth.truncated && zeroWidth.lines.size() == 7);
  for (const auto& line : zeroWidth.lines) assert(line.size() <= 243);
  std::cout << "UTF-8 validation, wrapping, newlines, clipping and work limits passed\n";
}
