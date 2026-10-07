#include "history_ui.h"
#include "message_text.h"
#include <cassert>
#include <string>

using namespace history_ui;
void tap(State& state, int x, int y) { state.press(x, y); state.release(x, y); }
void swipe(State& state, int from, int to) { state.press(400, from); state.move(400, to); state.release(400, to); }
int main() {
  State state;
  std::vector<uint32_t> ids;
  for (uint32_t i = 1; i <= 20; ++i) ids.push_back(i);
  state.sync(ids);
  assert(!state.dirty());
  tap(state, 400, 100); assert(state.screen() == Screen::DASHBOARD);
  state.press(400, 260); state.move(500, 260); state.release(400, 260);
  assert(state.screen() == Screen::DASHBOARD);  // A drag out and back is not a tap.
  tap(state, 400, 260); assert(state.screen() == Screen::LIST && state.dirty());
  state.rendered();
  swipe(state, 400, 190); assert(state.offset() == 3 && state.dirty());
  tap(state, 850, 510); assert(state.offset() == 9);
  swipe(state, 470, 65); assert(state.offset() == 14);  // Last full page.
  swipe(state, 470, 65); assert(state.offset() == 14);
  swipe(state, 100, 380); assert(state.offset() == 10);
  const auto anchor = ids[ids.size() - 1 - state.offset()];
  ids.push_back(21); state.sync(ids);
  assert(ids[ids.size() - 1 - state.offset()] == anchor);
  tap(state, 400, 80);
  assert(state.screen() == Screen::DETAIL && state.selected() == anchor);
  state.setDetailLines(100);
  swipe(state, 400, 150); assert(state.detailOffset() == 10);
  tap(state, 850, 510); assert(state.detailOffset() == 26);
  state.setDetailLines(20); assert(state.detailOffset() == 4);
  tap(state, 60, 30); assert(state.screen() == Screen::LIST);
  tap(state, 60, 30); assert(state.screen() == Screen::DASHBOARD);
  tap(state, 400, 260); assert(state.offset() == 0);
  tap(state, 400, 80); assert(state.selected() == 21);
  ids.pop_back(); state.sync(ids); assert(state.screen() == Screen::LIST);
  state.sync({}); assert(state.offset() == 0 && state.count() == 0);
  swipe(state, 400, 100); assert(state.offset() == 0);
  tap(state, 400, 80); assert(state.screen() == Screen::LIST);
  state.press(60, 30); state.cancelTouch(); state.release(60, 30);
  assert(state.screen() == Screen::LIST);
  tap(state, 60, 30); assert(state.screen() == Screen::DASHBOARD);

  const auto measure = [](const std::string& line) { return int(line.size()); };
  auto page = message_text::wrapPage("abcdefghi", 3, 1, 1, measure);
  assert(page.totalLines == 3 && page.lines == std::vector<std::string>({"def"}));
  page = message_text::wrapPage("a\r\nb\rc\n", 10, 0, 16, measure);
  assert(page.totalLines == 4 && page.lines.back().empty());
  page = message_text::wrapPage(std::string(4096, '\n'), 900, 4081, 16, measure);
  assert(page.totalLines == 4097 && page.lines.size() == 16);
  page = message_text::wrapPage("日本語日本語", 9, 1, 16, measure);
  assert(page.totalLines == 2 && page.lines[0] == "日本語");
  page = message_text::wrapPage("😀\tX", 100, 0, 16, measure);
  assert(page.lines[0] == "□    X");
}
