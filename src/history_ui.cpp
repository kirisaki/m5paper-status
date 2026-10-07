#include "history_ui.h"
#include <algorithm>
#include <cstdlib>

namespace history_ui {
void State::sync(const std::vector<uint32_t>& ids) {
  if (ids == ids_) return;
  const uint32_t anchor = offset_ && offset_ < ids_.size() ? ids_[ids_.size() - 1 - offset_] : 0;
  ids_ = ids;
  if (anchor && !ids_.empty()) {
    auto it = std::upper_bound(ids_.begin(), ids_.end(), anchor);
    offset_ = it == ids_.begin() ? ids_.size() - 1 : ids_.end() - it;
  }
  offset_ = std::min(offset_, ids_.size() > kRows ? ids_.size() - kRows : 0);
  if (screen_ == Screen::DETAIL && !std::binary_search(ids_.begin(), ids_.end(), selected_)) {
    screen_ = Screen::LIST; selected_ = 0; detailOffset_ = 0;
  }
  if (screen_ != Screen::DASHBOARD) dirty_ = true;
}
void State::press(int x, int y) {
  touching_ = true; startX_ = x; startY_ = y; travel_ = 0;
}
void State::move(int x, int y) {
  if (touching_) travel_ = std::max(travel_, std::max(std::abs(x - startX_), std::abs(y - startY_)));
}
void State::scroll(int amount) {
  if (screen_ == Screen::DASHBOARD) return;
  size_t& offset = screen_ == Screen::LIST ? offset_ : detailOffset_;
  const size_t count = screen_ == Screen::LIST ? ids_.size() : detailLines_;
  const size_t visible = screen_ == Screen::LIST ? kRows : kDetailRows;
  const int maximum = count > visible ? count - visible : 0;
  const size_t next = std::max(0, std::min(maximum, int(offset) + amount));
  if (next != offset) { offset = next; dirty_ = true; }
}
void State::back() {
  cancelTouch();
  if (screen_ == Screen::DASHBOARD) return;
  screen_ = screen_ == Screen::DETAIL ? Screen::LIST : Screen::DASHBOARD;
  selected_ = 0; detailOffset_ = 0; dirty_ = true;
}
void State::setDetailLines(size_t count) {
  detailLines_ = count;
  detailOffset_ = std::min(detailOffset_, count > kDetailRows ? count - kDetailRows : 0);
}
void State::release(int x, int y) {
  if (!touching_) return;
  move(x, y); touching_ = false;
  const int dx = x - startX_, dy = y - startY_;
  if (screen_ != Screen::DASHBOARD && startY_ >= kHeader && startY_ < kFooter
      && std::abs(dy) >= 32 && std::abs(dy) > std::abs(dx)) {
    const int step = screen_ == Screen::LIST ? kRowHeight : kDetailStep;
    const int page = screen_ == Screen::LIST ? kRows : kDetailRows;
    const int amount = std::max(1, std::min(page, std::abs(dy) / step));
    scroll(dy < 0 ? amount : -amount);
    return;
  }
  if (travel_ > 14 || x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  if (screen_ == Screen::DASHBOARD) {
    if (y >= 216 && y < 353) { screen_ = Screen::LIST; offset_ = 0; dirty_ = true; }
    return;
  }
  if (y < kHeader && x < 144) { back(); return; }
  if (y >= kFooter) {
    const int page = screen_ == Screen::LIST ? kRows : kDetailRows;
    if (x < 240) scroll(-page);
    else if (x >= kWidth - 240) scroll(page);
    return;
  }
  if (screen_ == Screen::LIST && y >= kHeader) {
    const size_t index = offset_ + (y - kHeader) / kRowHeight;
    if (index < ids_.size()) {
      selected_ = ids_[ids_.size() - 1 - index];
      screen_ = Screen::DETAIL; detailOffset_ = 0; detailLines_ = 0; dirty_ = true;
    }
  }
}
}  // namespace history_ui
