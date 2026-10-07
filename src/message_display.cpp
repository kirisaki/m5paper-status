#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <OpenFontRender.h>

#include <cstdio>
#include <algorithm>
#include <array>
#include <ctime>

#include "message_display.h"
#include "message_text.h"
#include "config.h"

// SD.begin mounts the card at /sd. Use ESP32 VFS stdio so FreeType gets
// standard seek/read semantics and releases every file when changing weights.
FT_FILE* OFR_fopen(const char* filename, const char* mode) {
  const String path = String("/sd") + filename;
  return std::fopen(path.c_str(), mode);
}

void OFR_fclose(FT_FILE* stream) {
  std::fclose(static_cast<FILE*>(stream));
}

size_t OFR_fread(void* ptr, size_t size, size_t count, FT_FILE* stream) {
  return std::fread(ptr, size, count, static_cast<FILE*>(stream));
}

int OFR_fseek(FT_FILE* stream, long offset, int whence) {
  return std::fseek(static_cast<FILE*>(stream), offset, whence);
}

long OFR_ftell(FT_FILE* stream) {
  return std::ftell(static_cast<FILE*>(stream));
}

namespace {
constexpr int kCalendarBottom = 216;
constexpr int kCalendarNoticeTop = kCalendarBottom - 25;
constexpr int kMessageTop = 240;
constexpr int kUsageTop = 372;
constexpr int kUsageRowStep = 52;
constexpr int kEventsTop = 68;
constexpr int kEventStep = 24;
constexpr unsigned kEventLanes = 5;

enum FontRole { REGULAR, BOLD };
struct Font {
  const char* filename;
  const char* label;
  String path;
  uint8_t* data;
  size_t size;
};

Font configuredFonts[] = {
    {config::kFontRegular, "REGULAR", "", nullptr, 0},
    {config::kFontBold, "BOLD", "", nullptr, 0},
};
M5Canvas canvas(&M5.Display);
bool canvasReady = false;
bool cardReady = false;
bool messageTruncated = false;
bool usingRegular = false;

String normalizedFontName(String name) {
  name.toLowerCase();
  name.replace("-", "");
  name.replace("_", "");
  name.replace(" ", "");
  return name;
}

void discoverFonts(const char* directory, uint8_t depth) {
  File dir = SD.open(directory);
  if (!dir || !dir.isDirectory()) {
    return;
  }
  while (File entry = dir.openNextFile()) {
    String name = entry.name();
    if (name.startsWith(".")) {
      continue;
    }
    const String path = entry.path();
    if (entry.isDirectory()) {
      entry.close();
      if (depth < 4) {
        discoverFonts(path.c_str(), depth + 1);
      }
      continue;
    }
    entry.close();
    name.toLowerCase();
    if (!name.endsWith(".ttf")) {
      continue;
    }
    name = normalizedFontName(name);
    for (auto& font : configuredFonts) {
      if (name == normalizedFontName(font.filename) && font.path.isEmpty()) {
        font.path = path;
        Serial.printf("Font found: %s -> %s\n", font.label, path.c_str());
      }
    }
  }
}

bool loadFont(OpenFontRender& renderer, FontRole role, unsigned size) {
  renderer.setDrawer(canvas);
  renderer.setUseRenderTask(false);
  renderer.setCacheSize(1, 1, 64 * 1024);
  const auto& font = configuredFonts[role];
  if (font.data) {
    const auto error = renderer.loadFont(font.data, font.size);
    if (error) {
      Serial.printf("Font load failed: %s, error=%d\n", font.label, error);
      renderer.unloadFont();
      return false;
    }
    renderer.setFontSize(size);
    renderer.setFontColor(TFT_BLACK, TFT_WHITE);
    return true;
  }
  return false;
}

void cacheFonts() {
  // Two Japanese TTFs fit in the 8 MB PSRAM. Keep SD and EPD bus traffic
  // entirely separate, including while TLS runs on the other CPU task.
  for (auto& font : configuredFonts) {
    if (font.path.isEmpty()) continue;
    File file = SD.open(font.path.c_str());
    if (!file || !file.size() || file.size() > 3 * 1024 * 1024) continue;
    const size_t size = file.size();
    auto* data = static_cast<uint8_t*>(ps_malloc(size));
    if (!data) { Serial.println("Font PSRAM allocation failed."); continue; }
    const size_t read = file.read(data, size);
    file.close();
    if (read != size) { free(data); Serial.println("Font SD read failed."); continue; }
    font.data = data;
    font.size = size;
    Serial.printf("Font cached in PSRAM: %s, %u bytes\n", font.label, static_cast<unsigned>(size));
  }
}

}  // namespace

namespace message_display {
void begin() {
  Serial.println("Preparing message canvas...");
  canvas.setPsram(true);
  canvas.setColorDepth(4);
  canvasReady = canvas.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
  if (!canvasReady) {
    Serial.println("Message canvas allocation failed.");
    return;
  }
  canvas.setPaletteGrayscale();
  Serial.printf("Message canvas: %d x %d\n", canvas.width(), canvas.height());
  SPI.begin(14, 13, 12, 4);
  M5.Display.waitDisplay();
  cardReady = SD.begin(4, SPI, 10000000, "/sd", 8);
  Serial.printf("SD: %s\n", cardReady ? "mounted" : "mount failed");
  if (cardReady) {
    discoverFonts("/", 0);
    cacheFonts();
  }
}

void drawDashboard(const std::string& text, const calendar::Snapshot& schedule, const usage::Snapshot& usageState) {
  if (!canvasReady) return;
  canvas.fillScreen(TFT_WHITE);
  canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  canvas.setFont(&fonts::lgfxJapanGothic_20);
  const int width = canvas.width();
  const auto column = [width](unsigned day) { return static_cast<int>(day * width / 7); };
  const uint16_t rule = 0xBDF7;
  const uint16_t band = 0xDEFB;

  OpenFontRender heading;
  const bool headingFont = loadFont(heading, BOLD, 22);
  for (unsigned day = 0; day < 7; ++day) {
    const int left = column(day);
    const int center = (left + column(day + 1)) / 2;
    std::string weekday = "—";
    std::string date = "--";
    if (schedule.clockReady) {
      const auto label = calendar::dateLabel(schedule.today + day * calendar::kDay);
      const auto space = label.find(' ');
      weekday = label.substr(space + 1);
      date = label.substr(0, space);
    }
    const auto centered = [&](const std::string& value, int y, uint16_t fg, uint16_t bg) {
      int textWidth = canvas.textWidth(value.c_str());
      if (headingFont) {
        const auto box = heading.calculateBoundingBox(0, 0, 22, Align::Left, Layout::Horizontal, value.c_str());
        textWidth = box.xMax - box.xMin;
        heading.drawString(value.c_str(), center - textWidth / 2, y, fg, bg);
      } else {
        canvas.setTextColor(fg, bg);
        canvas.setCursor(center - textWidth / 2, y); canvas.print(value.c_str());
      }
    };
    centered(weekday, 4, TFT_BLACK, TFT_WHITE);
    if (schedule.clockReady && day == 0) {
      // Circle the day number; include the month on other dates at boundaries.
      date = date.substr(date.find('/') + 1);
      canvas.fillCircle(center, 44, 17, TFT_BLACK);
      centered(date, 31, TFT_WHITE, TFT_BLACK);
    } else {
      centered(date, 31, TFT_BLACK, TFT_WHITE);
    }
    if (day) canvas.drawFastVLine(left, 0, kCalendarBottom, rule);
  }
  if (headingFont) heading.unloadFont();
  canvas.setTextColor(TFT_BLACK, TFT_WHITE);
  canvas.drawFastHLine(0, kCalendarBottom, width, rule);

  OpenFontRender body;
  usingRegular = loadFont(body, REGULAR, 20);
  const auto measure = [&](const std::string& value, unsigned size) -> int {
    if (!usingRegular) return canvas.textWidth(value.c_str());
    const auto box = body.calculateBoundingBox(0, 0, size, Align::Left, Layout::Horizontal, value.c_str());
    return box.xMax - box.xMin;
  };
  const auto drawText = [&](const std::string& value, int x, int y, int w,
                            unsigned size, unsigned lines, uint16_t background) -> bool {
    if (usingRegular) body.setFontSize(size);
    const auto layout = message_text::wrap(value, w, lines,
        [&](const std::string& line) { return measure(line, size); });
    const int step = size + 4;
    canvas.setClipRect(x, y, w, step * lines);
    for (size_t i = 0; i < layout.lines.size(); ++i) {
      if (usingRegular) body.drawString(layout.lines[i].c_str(), x, y + step * i, TFT_BLACK, background);
      else {
        canvas.setTextColor(TFT_BLACK, background);
        canvas.setCursor(x, y + step * i); canvas.print(layout.lines[i].c_str());
      }
    }
    canvas.clearClipRect();
    return layout.truncated;
  };

  // Reserve the same lane across days so all-day bars stay continuous.
  std::array<std::array<bool, kEventLanes>, 7> occupied{};
  std::array<unsigned, 7> hidden{};
  std::vector<const calendar::Event*> ordered;
  for (const auto& event : schedule.events) ordered.push_back(&event);
  std::stable_sort(ordered.begin(), ordered.end(), [](const calendar::Event* a, const calendar::Event* b) {
    if (a->allDay != b->allDay) return a->allDay;
    return a->start < b->start;
  });
  const auto reserve = [&](unsigned first, unsigned last, unsigned rows) -> int {
    for (unsigned lane = 0; lane + rows <= kEventLanes; ++lane) {
      bool fits = true;
      for (unsigned d = first; d <= last; ++d)
        for (unsigned r = 0; r < rows; ++r) if (occupied[d][lane + r]) fits = false;
      if (!fits) continue;
      for (unsigned d = first; d <= last; ++d)
        for (unsigned r = 0; r < rows; ++r) occupied[d][lane + r] = true;
      return lane;
    }
    return -1;
  };
  if (schedule.clockReady) for (const auto* event : ordered) {
    unsigned first = 7, last = 0;
    for (unsigned d = 0; d < 7; ++d) {
      const int64_t date = schedule.today + d * calendar::kDay;
      if (event->start < date + calendar::kDay && event->end > date) {
        first = std::min(first, d); last = d;
      }
    }
    if (first == 7) continue;
    if (event->allDay) {
      const int lane = reserve(first, last, 1);
      if (lane < 0) { for (unsigned d = first; d <= last; ++d) ++hidden[d]; continue; }
      const int x = column(first) + 3;
      const int w = column(last + 1) - x - 3;
      const int y = kEventsTop + lane * kEventStep;
      canvas.fillRoundRect(x, y, w, 22, 5, band);
      drawText(event->title, x + 5, y, w - 10, 20, 1, band);
    } else {
      for (unsigned d = first; d <= last; ++d) {
        const auto label = calendar::timeLabel(*event, schedule.today + d * calendar::kDay) + " " + event->title;
        const int x = column(d) + 6;
        const int w = column(d + 1) - x - 6;
        const unsigned rows = message_text::wrap(label, w, 2,
            [&](const std::string& line) { return measure(line, 20); }).lines.size() > 1 ? 2 : 1;
        const int lane = reserve(d, d, rows);
        if (lane < 0) { ++hidden[d]; continue; }
        drawText(label, x, kEventsTop + lane * kEventStep, w, 20, rows, TFT_WHITE);
      }
    }
  }
  for (unsigned day = 0; day < 7; ++day) if (hidden[day]) {
    drawText("他 " + std::to_string(hidden[day]) + " 件", column(day) + 6, kCalendarNoticeTop,
             column(day + 1) - column(day) - 12, 18, 1, TFT_WHITE);
  }
  std::string calendarNotice;
  if (!schedule.configured) calendarNotice = "カレンダー未設定";
  else if (!schedule.clockReady) calendarNotice = "時刻同期中";
  else if (!schedule.error.empty()) calendarNotice = schedule.ready ? "更新失敗・前回の予定を表示中" : "カレンダーを取得できません";
  else if (!schedule.ready) calendarNotice = "予定を取得中";
  else if (schedule.limited) calendarNotice = "取得上限に達しました";
  if (!calendarNotice.empty()) {
    // Only exceptional states need a notice; leave a healthy calendar headerless.
    canvas.fillRect(0, kCalendarNoticeTop, width, 24, TFT_WHITE);
    drawText(calendarNotice, 8, kCalendarNoticeTop, width - 16, 18, 1, TFT_WHITE);
  }

  // Message content only. An empty message leaves this space blank.
  messageTruncated = drawText(text, 16, kMessageTop, width - 32, 26, 3, TFT_WHITE);
  canvas.drawFastHLine(16, kUsageTop - 19, width - 32, rule);
  const int64_t now = time(nullptr);
  const auto graph = [&](const usage::Provider& provider,
                         const usage::Window& window, unsigned row, unsigned col) {
    const int x = col * width / 2 + 16;
    const int y = kUsageTop + row * kUsageRowStep;
    const int w = width / 2 - 32;
    drawText(usage::resetLabel(provider, window, now), x + 206, y + 2, w - 206, 16, 1, TFT_WHITE);
    const int barWidth = w - 66;
    canvas.drawRect(x, y + 27, barWidth, 18, TFT_BLACK);
    const bool valid = provider.fetchedAt && window.available && !usage::expired(window, now);
    if (valid) {
      const int fill = usage::fillWidth(window, barWidth - 4);
      if (fill) canvas.fillRect(x + 2, y + 29, fill, 14, usage::stale(provider, now) ? rule : TFT_BLACK);
    }
    char percent[24];
    if (valid) std::snprintf(percent, sizeof(percent), "%.0f%%", window.usedPercent);
    else std::snprintf(percent, sizeof(percent), "--");
    drawText(percent, x + barWidth + 8, y + 24, 58, 18, 1, TFT_WHITE);
  };
  graph(usageState.codex, usageState.codex.session, 0, 0);
  graph(usageState.claude, usageState.claude.session, 0, 1);
  graph(usageState.codex, usageState.codex.weekly, 1, 0);
  graph(usageState.claude, usageState.claude.weekly, 1, 1);
  bool hasFable = false;
  for (const auto& model : usageState.claude.models) {
    if (model.name == "Fable") { graph(usageState.claude, model.window, 2, 1); hasFable = true; break; }
  }
  if (usingRegular) body.unloadFont();
  // Draw bold titles in a separate pass to keep only one FreeType face/cache
  // active at a time, leaving internal RAM available for the ESP32 runtime.
  const bool titleFont = loadFont(heading, BOLD, 20);
  const auto title = [&](const char* label, unsigned row, unsigned col) {
    const int x = col * width / 2 + 16;
    const int y = kUsageTop + row * kUsageRowStep;
    canvas.setClipRect(x, y, 202, 24);
    if (titleFont) heading.drawString(label, x, y, TFT_BLACK, TFT_WHITE);
    else {
      canvas.setTextColor(TFT_BLACK, TFT_WHITE);
      canvas.setCursor(x, y); canvas.print(label);
    }
    canvas.clearClipRect();
  };
  title("Codex セッション", 0, 0);
  title("Claude セッション", 0, 1);
  title("Codex 週間", 1, 0);
  title("Claude 週間", 1, 1);
  if (hasFable) title("Fable 週間", 2, 1);
  if (titleFont) heading.unloadFont();
  Serial.printf("Dashboard rendered: 7 columns, calendar=%u events, usage=live\n",
                static_cast<unsigned>(schedule.events.size()));
}

void present() {
  if (!canvasReady) {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setFont(&fonts::lgfxJapanGothic_24);
    M5.Display.setCursor(24, 24);
    M5.Display.println("表示用メモリの確保に失敗");
  } else {
    canvas.pushSprite(0, 0);
  }
  M5.Display.display();
}

bool sdReady() { return cardReady; }
bool truncated() { return messageTruncated; }
const char* bodyFont() { return usingRegular ? config::kFontRegular : "builtin"; }
}  // namespace message_display
