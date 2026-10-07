#include "calendar_service.h"
#include "config.h"
#include "message_text.h"
#include "work_gate.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <esp_system.h>
#include <esp_sntp.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <memory>

extern const char googleRoots[] asm("_binary_certs_google_roots_pem_start");

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
calendar::Snapshot current;
calendar::Snapshot delivered;
SemaphoreHandle_t mutex = nullptr;
bool deliveryPending = false;
bool redraw = true;
std::atomic<bool> clockSynchronized{false};
String accessToken;
int64_t tokenExpiry = 0;
constexpr size_t kMaxEvents = 200;

const char* stringValue(const cJSON* object, const char* name) {
  const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
String jsonString(cJSON* object) {
  char* text = cJSON_PrintUnformatted(object);
  String result(text ? text : "{}");
  cJSON_free(text);
  return result;
}
String encoded(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += char(c);
    else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}
String base64url(const unsigned char* input, size_t size) {
  std::vector<unsigned char> output(4 * ((size + 2) / 3) + 1);
  size_t length = 0;
  if (mbedtls_base64_encode(output.data(), output.size(), &length, input, size)) return "";
  String result(reinterpret_cast<const char*>(output.data()), length);
  result.replace("+", "-"); result.replace("/", "_"); result.replace("=", "");
  return result;
}
String base64url(const String& value) {
  return base64url(reinterpret_cast<const unsigned char*>(value.c_str()), value.length());
}
int randomBytes(void*, unsigned char* output, size_t size) {
  esp_fill_random(output, size);
  return 0;
}
String assertion(int64_t now) {
  Json claims(cJSON_CreateObject(), cJSON_Delete);
  if (!claims) return "";
  cJSON_AddStringToObject(claims.get(), "iss", config::kCalendarEmail);
  cJSON_AddStringToObject(claims.get(), "scope", "https://www.googleapis.com/auth/calendar.events.readonly");
  cJSON_AddStringToObject(claims.get(), "aud", "https://oauth2.googleapis.com/token");
  cJSON_AddNumberToObject(claims.get(), "iat", now);
  cJSON_AddNumberToObject(claims.get(), "exp", now + 3600);
  String input = base64url(String("{\"alg\":\"RS256\",\"typ\":\"JWT\"}")) + "." + base64url(jsonString(claims.get()));
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  int error = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char*>(config::kCalendarPrivateKey),
                                  strlen(config::kCalendarPrivateKey) + 1, nullptr, 0);
  unsigned char hash[32];
  unsigned char signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
  size_t length = 0;
  if (!error && !mbedtls_pk_can_do(&key, MBEDTLS_PK_RSA)) error = -1;
  if (!error) error = mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash, 0);
  if (!error) error = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, &length, randomBytes, nullptr);
  mbedtls_pk_free(&key);
  return error ? String("") : input + "." + base64url(signature, length);
}

// HTTPClient decodes chunked bodies into this bounded sink.
class BoundedBody : public Stream {
 public:
  explicit BoundedBody(size_t limit) : limit_(limit) {}
  std::string value;
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (size > limit_ - value.size()) return 0;
    value.append(reinterpret_cast<const char*>(bytes), size);
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
 private:
  size_t limit_;
};

int request(const String& url, const String& form, size_t limit, std::string& body) {
  WiFiClientSecure client;
  client.setCACert(googleRoots);
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(client, url)) return -1;
  int status;
  if (form.isEmpty()) {
    http.addHeader("Authorization", "Bearer " + accessToken);
    status = http.GET();
  } else {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    status = http.POST(form);
  }
  if (status > 0) {
    const size_t bodyLimit = status == 200 ? limit : 8192;
    BoundedBody sink(bodyLimit);
    if (http.getSize() > static_cast<int>(bodyLimit) || http.writeToStream(&sink) < 0) {
      if (status == 200) status = -2;
    }
    else body = std::move(sink.value);
  }
  http.end();
  return status;
}

std::string errorReason(const std::string& body) {
  Json json(cJSON_Parse(body.c_str()), cJSON_Delete);
  const auto* error = cJSON_GetObjectItemCaseSensitive(json.get(), "error");
  const auto* errors = cJSON_GetObjectItemCaseSensitive(error, "errors");
  const char* reason = cJSON_IsString(error) ? error->valuestring : stringValue(cJSON_GetArrayItem(errors, 0), "reason");
  if (!reason || !*reason || strlen(reason) > 64) return "";
  // Return only a machine-readable reason, never Google's free-form message.
  for (const char* p = reason; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return "";
  }
  return std::string("_") + reason;
}

bool authorize(int64_t now, std::string& error) {
  if (!accessToken.isEmpty() && tokenExpiry > now + 60) return true;
  accessToken = "";
  const String jwt = assertion(now);
  if (jwt.isEmpty()) { error = "invalid_private_key"; return false; }
  std::string body;
  const int code = request("https://oauth2.googleapis.com/token",
      "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt, 8192, body);
  if (code != 200) { error = "auth_http_" + std::to_string(code) + errorReason(body); return false; }
  Json json(cJSON_Parse(body.c_str()), cJSON_Delete);
  const auto* expires = cJSON_GetObjectItemCaseSensitive(json.get(), "expires_in");
  const char* token = stringValue(json.get(), "access_token");
  if (!*token || strlen(token) > 4096 || !cJSON_IsNumber(expires) || expires->valuedouble < 120 || expires->valuedouble > 86400) {
    error = "invalid_token_response"; return false;
  }
  accessToken = token;
  tokenExpiry = now + static_cast<int64_t>(expires->valuedouble);
  return true;
}

bool fetch(calendar::Snapshot& result) {
  const int64_t now = time(nullptr);
  if (!authorize(now, result.error)) return false;
  const int64_t today = calendar::startOfDay(now);
  const String base = String("https://www.googleapis.com/calendar/v3/calendars/") + encoded(config::kCalendarId) +
      "/events?singleEvents=true&orderBy=startTime&showDeleted=false&maxResults=50&timeZone=Asia%2FTokyo&timeMin=" +
      encoded(calendar::rfc3339(today).c_str()) + "&timeMax=" + encoded(calendar::rfc3339(today + 7 * calendar::kDay).c_str()) +
      "&fields=nextPageToken,items(status,summary,start,end)";
  std::vector<calendar::Event> events;
  String pageToken;
  bool limited = false;
  for (unsigned page = 0; page < 4; ++page) {
    std::string body;
    const int code = request(base + (pageToken.isEmpty() ? String("") : "&pageToken=" + encoded(pageToken)), "", 64 * 1024, body);
    if (code != 200) {
      if (code == 401) { accessToken = ""; tokenExpiry = 0; }
      result.error = "calendar_http_" + std::to_string(code) + errorReason(body); return false;
    }
    Json json(cJSON_Parse(body.c_str()), cJSON_Delete);
    const auto* items = cJSON_GetObjectItemCaseSensitive(json.get(), "items");
    if (!cJSON_IsArray(items)) { result.error = "invalid_calendar_response"; return false; }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, items) {
      if (!strcmp(stringValue(item, "status"), "cancelled")) continue;
      calendar::Event event;
      const auto* start = cJSON_GetObjectItemCaseSensitive(item, "start");
      const auto* end = cJSON_GetObjectItemCaseSensitive(item, "end");
      const char* date = stringValue(start, "date");
      event.allDay = *date;
      const bool valid = event.allDay
          ? calendar::parseDate(date, event.start) && calendar::parseDate(stringValue(end, "date"), event.end)
          : calendar::parseDateTime(stringValue(start, "dateTime"), event.start) && calendar::parseDateTime(stringValue(end, "dateTime"), event.end);
      if (!valid || event.end < event.start) { result.error = "invalid_event_time"; return false; }
      // Zero-duration timed events still belong to their start day.
      if (event.end == event.start) ++event.end;
      event.title = stringValue(item, "summary");
      if (event.title.empty()) event.title = "（タイトルなし）";
      if (!message_text::valid(event.title)) event.title = "（表示できないタイトル）";
      for (auto& c : event.title) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
      if (event.title.size() > 512) {
        size_t cut = 509;
        while ((static_cast<unsigned char>(event.title[cut]) & 0xC0) == 0x80) --cut;
        event.title.resize(cut); event.title += "…";
      }
      events.push_back(std::move(event));
      if (events.size() >= kMaxEvents) { limited = true; break; }
    }
    pageToken = stringValue(json.get(), "nextPageToken");
    if (pageToken.isEmpty() || limited) break;
    if (pageToken.length() > 2048) { result.error = "invalid_page_token"; return false; }
    if (page == 3) limited = true;
  }
  result.events = std::move(events);
  result.today = today;
  result.fetchedAt = now;
  result.ready = true;
  result.limited = limited;
  result.error.clear();
  return true;
}

void publish(const calendar::Snapshot& value) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  delivered = value;
  deliveryPending = true;
  xSemaphoreGive(mutex);
}
void worker(void*) {
  calendar::Snapshot state;
  state.configured = true;
  uint32_t lastAttempt = 0;
  uint32_t interval = 0;
  bool attempted = false;
  int64_t attemptedDay = 0;
  for (;;) {
    const int64_t now = time(nullptr);
    const bool clockReady = clockSynchronized.load() && now >= 1704067200;
    if (state.clockReady != clockReady) { state.clockReady = clockReady; publish(state); }
    if (clockReady && WiFi.status() == WL_CONNECTED) {
      const int64_t today = calendar::startOfDay(now);
      if (!attempted || millis() - lastAttempt >= interval || attemptedDay != today) {
        attempted = true;
        attemptedDay = today;
        state.today = today;
        work_gate::lock();
        const bool success = fetch(state);
        work_gate::unlock();
        lastAttempt = millis();
        interval = success ? config::kCalendarRefreshSeconds * 1000UL : 60000UL;
        Serial.printf("Calendar: %s, events=%u\n", success ? "updated" : state.error.c_str(), static_cast<unsigned>(state.events.size()));
        publish(state);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
}  // namespace

namespace calendar_service {
void begin(WebServer& server) {
  current.configured = config::kCalendarId[0] != '\0';
  sntp_set_time_sync_notification_cb([](struct timeval*) { clockSynchronized.store(true); });
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  if (current.configured) {
    mutex = xSemaphoreCreateMutex();
    if (!mutex || !work_gate::begin() || xTaskCreate(worker, "calendar", 16384, nullptr, 1, nullptr) != pdPASS) current.error = "worker_start_failed";
  }
  server.on("/api/calendar", HTTP_GET, [&server]() {
    const String token(config::apiToken("device"));
    if (!token.isEmpty() && server.header("Authorization") != "Bearer " + token) {
      server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return;
    }
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json) { server.send(503, "application/json", "{\"error\":\"no_memory\"}"); return; }
    cJSON_AddBoolToObject(json.get(), "configured", current.configured);
    cJSON_AddBoolToObject(json.get(), "clock_ready", current.clockReady);
    cJSON_AddBoolToObject(json.get(), "ready", current.ready);
    cJSON_AddBoolToObject(json.get(), "limited", current.limited);
    cJSON_AddBoolToObject(json.get(), "stale", current.ready && (!current.error.empty() || time(nullptr) - current.fetchedAt > config::kCalendarRefreshSeconds + 60));
    cJSON_AddNumberToObject(json.get(), "fetched_at", current.fetchedAt);
    cJSON_AddStringToObject(json.get(), "error", current.error.c_str());
    cJSON_AddStringToObject(json.get(), "time_zone", "Asia/Tokyo");
    auto* items = cJSON_AddArrayToObject(json.get(), "events");
    for (const auto& event : current.events) {
      auto* item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "title", event.title.c_str());
      cJSON_AddStringToObject(item, "start", calendar::rfc3339(event.start).c_str());
      cJSON_AddStringToObject(item, "end", calendar::rfc3339(event.end).c_str());
      cJSON_AddBoolToObject(item, "all_day", event.allDay);
      cJSON_AddItemToArray(items, item);
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", jsonString(json.get()));
  });
}
void tick() {
  if (mutex && xSemaphoreTake(mutex, 0) == pdTRUE) {
    if (deliveryPending) { current = delivered; deliveryPending = false; redraw = true; }
    xSemaphoreGive(mutex);
  }
  const int64_t now = time(nullptr);
  const bool clockReady = clockSynchronized.load() && now >= 1704067200;
  const int64_t today = clockReady ? calendar::startOfDay(now) : 0;
  if (current.clockReady != clockReady || current.today != today) {
    current.clockReady = clockReady; current.today = today; redraw = true;
  }
}
bool pending() { return redraw; }
void rendered() { redraw = false; }
const calendar::Snapshot& snapshot() { return current; }
bool tryBeginRender() { return work_gate::tryLock(); }
void endRender() { work_gate::unlock(); }
}  // namespace calendar_service
