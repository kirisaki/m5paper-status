#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace calendar {
constexpr int64_t kDay = 86400;
constexpr int64_t kJst = 9 * 3600;
struct Event {
  std::string title;
  int64_t start = 0;
  int64_t end = 0;  // Exclusive, including all-day events.
  bool allDay = false;
};
struct Snapshot {
  bool configured = false;
  bool clockReady = false;
  bool ready = false;
  bool limited = false;
  int64_t today = 0;
  int64_t fetchedAt = 0;
  std::string error;
  std::vector<Event> events;
};
bool parseDate(const std::string& value, int64_t& seconds);
bool parseDateTime(const std::string& value, int64_t& seconds);
int64_t startOfDay(int64_t now);
std::string dateLabel(int64_t day);
std::string timeLabel(const Event& event, int64_t day);
std::string rfc3339(int64_t seconds);
std::vector<const Event*> eventsOn(const Snapshot& snapshot, int64_t day);
}  // namespace calendar
