#include "calendar_model.h"
#include <cassert>
#include <string>

int main() {
  using namespace calendar;
  int64_t today, a, b;
  assert(parseDate("2026-10-07", today));
  assert(dateLabel(today) == "10/7 水");
  assert(rfc3339(today) == "2026-10-07T00:00:00+09:00");
  assert(parseDateTime("2026-10-06T15:00:00Z", a) && a == today);
  assert(parseDateTime("2026-10-07T00:00:00.123+09:00", a) && a == today);
  assert(parseDateTime("2026-10-06T08:00:00-07:00", a) && a == today);
  assert(startOfDay(today + kDay - 1) == today);
  assert(startOfDay(today + kDay) == today + kDay);
  assert(parseDate("2024-02-29", a));
  assert(!parseDate("2026-02-29", a));
  assert(!parseDate("2026-04-31", a));
  assert(!parseDate("2026-00-01", a));
  assert(!parseDate("2026-01-00", a));
  assert(!parseDate("2026-10-07x", a));
  assert(parseDate("2026-12-31", a) && parseDate("2027-01-01", b) && b-a == kDay);
  for (const char* s : {"2026-10-07T24:00:00Z", "2026-10-07T23:60:00Z",
       "2026-10-07T00:00:00", "2026-10-07T00:00:00+24:00", "2026-10-07T00:00:00+09:60",
       "2026-10-07T00:00:00.Z", "2026-10-07T00:00:00.", "2026-10-07T00:00:00Zextra"}) {
    assert(!parseDateTime(s, a));
  }
  Snapshot snapshot;
  Event allDay;
  allDay.title = "2日間の終日予定"; allDay.start = today; allDay.end = today + 2*kDay; allDay.allDay = true;
  Event overnight;
  overnight.title = "日またぎ"; overnight.start = today-3600; overnight.end = today+3600;
  Event midnightEnd;
  midnightEnd.start = today-3600; midnightEnd.end = today;
  Event timed;
  timed.start = today+10*3600; timed.end = timed.start+3600;
  snapshot.events = {timed, overnight, midnightEnd, allDay};
  auto items = eventsOn(snapshot, today);
  assert(items.size() == 3 && items[0]->allDay && items[1]->title == "日またぎ");
  assert(eventsOn(snapshot, today+kDay).size() == 1);
  assert(eventsOn(snapshot, today+2*kDay).empty());
  assert(timeLabel(allDay, today) == "終日");
  assert(timeLabel(overnight, today) == "継続");
  assert(timeLabel(timed, today) == "10:00");
}
