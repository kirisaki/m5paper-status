#include "calendar_model.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
bool digits(const std::string& s, size_t at, size_t n, int& out) {
  if (at + n > s.size()) return false;
  out = 0;
  for (size_t i = at; i < at + n; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    out = out * 10 + s[i] - '0';
  }
  return true;
}

// Gregorian civil date to Unix days; independent of the device's TZ setting.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = y - era * 400;
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  return era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
bool dateParts(const std::string& s, int64_t& utc) {
  int y, m, d;
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' ||
      !digits(s, 0, 4, y) || !digits(s, 5, 2, m) || !digits(s, 8, 2, d) ||
      y < 1970 || y > 2099 || m < 1 || m > 12) return false;
  const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int maxDay = lengths[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (d < 1 || d > maxDay) return false;
  utc = daysFromCivil(y, m, d) * calendar::kDay;
  return true;
}
std::tm jst(int64_t seconds) {
  const time_t t = seconds + calendar::kJst;
  std::tm value{};
  gmtime_r(&t, &value);
  return value;
}
}  // namespace

namespace calendar {
bool parseDate(const std::string& value, int64_t& seconds) {
  if (value.size() != 10 || !dateParts(value, seconds)) return false;
  seconds -= kJst;
  return true;
}

bool parseDateTime(const std::string& s, int64_t& seconds) {
  int h, m, sec;
  if (s.size() < 20 || !dateParts(s, seconds) || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
      !digits(s, 11, 2, h) || !digits(s, 14, 2, m) || !digits(s, 17, 2, sec) ||
      h > 23 || m > 59 || sec > 59) return false;
  size_t pos = 19;
  if (s[pos] == '.') {
    const size_t start = ++pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    if (pos == start) return false;
  }
  int offset = 0;
  if (pos + 1 == s.size() && s[pos] == 'Z') {
    // UTC
  } else {
    int oh, om;
    if (pos + 6 != s.size() || (s[pos] != '+' && s[pos] != '-') || s[pos + 3] != ':' ||
        !digits(s, pos + 1, 2, oh) || !digits(s, pos + 4, 2, om) || oh > 23 || om > 59) return false;
    offset = (oh * 60 + om) * 60 * (s[pos] == '-' ? -1 : 1);
  }
  seconds += h * 3600 + m * 60 + sec - offset;
  return true;
}

int64_t startOfDay(int64_t now) { return ((now + kJst) / kDay) * kDay - kJst; }

std::string dateLabel(int64_t day) {
  const auto t = jst(day);
  const char* weekdays[] = {"日", "月", "火", "水", "木", "金", "土"};
  char result[32];
  std::snprintf(result, sizeof(result), "%d/%d %s", t.tm_mon + 1, t.tm_mday, weekdays[t.tm_wday]);
  return result;
}

std::string timeLabel(const Event& event, int64_t day) {
  if (event.allDay) return "終日";
  if (event.start < day) return "継続";
  const auto t = jst(event.start);
  char result[16];
  std::snprintf(result, sizeof(result), "%02d:%02d", t.tm_hour, t.tm_min);
  return result;
}

std::string rfc3339(int64_t seconds) {
  const auto t = jst(seconds);
  char result[40];
  std::strftime(result, sizeof(result), "%Y-%m-%dT%H:%M:%S+09:00", &t);
  return result;
}

std::vector<const Event*> eventsOn(const Snapshot& snapshot, int64_t day) {
  std::vector<const Event*> found;
  for (const auto& event : snapshot.events) {
    if (event.start < day + kDay && event.end > day) found.push_back(&event);
  }
  std::stable_sort(found.begin(), found.end(), [](const Event* a, const Event* b) {
    if (a->allDay != b->allDay) return a->allDay;
    return a->start < b->start;
  });
  return found;
}
}  // namespace calendar
