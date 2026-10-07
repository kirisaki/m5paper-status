#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace history_ui {
constexpr int kWidth = 960;
constexpr int kHeight = 540;
constexpr int kHeader = 64;
constexpr int kRowHeight = 70;
constexpr size_t kRows = 6;
constexpr int kFooter = kHeader + kRowHeight * kRows;
constexpr size_t kDetailRows = 16;
constexpr int kDetailStep = 24;
enum class Screen { DASHBOARD, LIST, DETAIL };

class State {
 public:
  void sync(const std::vector<uint32_t>& ids);
  void press(int x, int y);
  void move(int x, int y);
  void release(int x, int y);
  void cancelTouch() { touching_ = false; }
  void back();
  void setDetailLines(size_t count);
  Screen screen() const { return screen_; }
  size_t offset() const { return offset_; }
  size_t count() const { return ids_.size(); }
  uint32_t selected() const { return selected_; }
  size_t detailOffset() const { return detailOffset_; }
  size_t detailLines() const { return detailLines_; }
  bool dirty() const { return dirty_; }
  bool touching() const { return touching_; }
  void rendered() { dirty_ = false; }
 private:
  Screen screen_ = Screen::DASHBOARD;
  std::vector<uint32_t> ids_;
  size_t offset_ = 0, detailOffset_ = 0, detailLines_ = 0;
  uint32_t selected_ = 0;
  bool dirty_ = false, touching_ = false;
  int startX_ = 0, startY_ = 0, travel_ = 0;
  void scroll(int amount);
};
}  // namespace history_ui
