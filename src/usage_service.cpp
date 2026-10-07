#include "usage_service.h"
#include "calendar_service.h"
#include "calendar_model.h"
#include "config.h"
#include "message_text.h"
#include "work_gate.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cJSON.h>
#include <atomic>
#include <cmath>
#include <ctime>
#include <memory>

extern const uint8_t rootsBundle[] asm("_binary_certs_roots_bundle_start");
extern const char googleRoots[] asm("_binary_certs_google_roots_pem_start");
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
usage::Snapshot current;
usage::Snapshot delivered;
SemaphoreHandle_t deliveryMutex = nullptr;
bool hasDelivery = false;
bool redraw = true;
std::atomic<bool> clockReady{false};
Preferences authStore;
bool storageReady = false;
const char* providerNames[] = {"codex", "claude"};
struct Auth { String access; String refresh; int64_t expires = 0; bool saveFailed = false; };
Auth auth[2];

const cJSON* field(const cJSON* object, const char* name) { return cJSON_GetObjectItemCaseSensitive(object, name); }
const char* str(const cJSON* object, const char* name) {
  const auto* value = field(object, name);
  return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
String serialize(const cJSON* json) {
  char* raw = cJSON_PrintUnformatted(json);
  String result(raw ? raw : "{}");
  cJSON_free(raw);
  return result;
}
bool tokenSafe(const char* text) {
  const size_t size = strlen(text);
  if (!size || size > 8192) return false;
  for (size_t i = 0; i < size; ++i) if (text[i] <= ' ' || text[i] > '~') return false;
  return true;
}
String encode(const String& input) {
  const char hex[] = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < input.length(); ++i) {
    const uint8_t c = input[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
class Body : public Stream {
 public:
  std::string value;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (size > 32768 - value.size()) return 0;
    value.append(reinterpret_cast<const char*>(data), size); return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Caller holds work_gate, so only one TLS client or font renderer uses the heap.
int request(unsigned provider, bool refresh, std::string& body, uint32_t& retryAfter) {
  WiFiClientSecure client;
  // The Arduino 2.x bundle verifier follows the server's cross-signed GTS
  // chain to a legacy root absent from Mozilla's current bundle. PEM trust
  // anchors let mbedTLS terminate that chain at the trusted GTS root itself.
  if (refresh) client.setCACertBundle(rootsBundle);
  else client.setCACert(googleRoots);
  client.setHandshakeTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(10000); http.setTimeout(10000); http.setReuse(false);
  const char* url = provider == 0
      ? (refresh ? "https://auth.openai.com/oauth/token" : "https://chatgpt.com/backend-api/wham/usage")
      : (refresh ? "https://platform.claude.com/v1/oauth/token" : "https://api.anthropic.com/api/oauth/usage");
  if (!http.begin(client, url)) return -1;
  const char* headers[] = {"Retry-After"}; http.collectHeaders(headers, 1);
  http.addHeader("User-Agent", "m5paper-status/0.1");
  int code;
  if (refresh) {
    const char* clientId = provider == 0 ? "app_EMoamEEZ73f0CkXaXp7hrann" : "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
    if (provider == 0) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      code = http.POST(String("grant_type=refresh_token&client_id=") + clientId + "&refresh_token=" + encode(auth[provider].refresh));
    } else {
      Json json(cJSON_CreateObject(), cJSON_Delete);
      cJSON_AddStringToObject(json.get(), "grant_type", "refresh_token");
      cJSON_AddStringToObject(json.get(), "client_id", clientId);
      cJSON_AddStringToObject(json.get(), "refresh_token", auth[provider].refresh.c_str());
      http.addHeader("Content-Type", "application/json");
      code = http.POST(serialize(json.get()));
    }
  } else {
    http.addHeader("Authorization", "Bearer " + auth[provider].access);
    if (provider == 0) http.addHeader("ChatGPT-Account-Id", config::kUsageCredentials[0].accountId);
    else http.addHeader("anthropic-beta", "oauth-2025-04-20");
    code = http.GET();
  }
  if (code == 429 || code == 503) {
    const long seconds = http.header("Retry-After").toInt();
    if (seconds > 0) retryAfter = std::min<long>(86400, seconds);
  }
  if (code > 0) {
    Body sink;
    if (http.getSize() > 32768 || http.writeToStream(&sink) < 0) { if (code == 200) code = -2; }
    else body = std::move(sink.value);
  }
  http.end();
  return code;
}

bool saveAuth(unsigned provider) {
  Json json(cJSON_CreateObject(), cJSON_Delete);
  cJSON_AddStringToObject(json.get(), "seed", config::kUsageCredentials[provider].seed);
  cJSON_AddStringToObject(json.get(), "access", auth[provider].access.c_str());
  cJSON_AddStringToObject(json.get(), "refresh", auth[provider].refresh.c_str());
  cJSON_AddNumberToObject(json.get(), "expires", auth[provider].expires);
  const String value = serialize(json.get());
  const bool saved = storageReady && authStore.putString(providerNames[provider], value) == value.length();
  auth[provider].saveFailed = !saved;
  return saved;
}
void loadAuth(unsigned provider) {
  auth[provider].refresh = config::kUsageCredentials[provider].refreshToken;
  if (!storageReady || auth[provider].refresh.isEmpty()) return;
  if (!authStore.isKey(providerNames[provider])) return;
  const String value = authStore.getString(providerNames[provider], "");
  if (value.length() > 16384) return;
  Json json(cJSON_Parse(value.c_str()), cJSON_Delete);
  if (strcmp(str(json.get(), "seed"), config::kUsageCredentials[provider].seed) != 0) return;
  const char* access = str(json.get(), "access");
  const char* refresh = str(json.get(), "refresh");
  const auto* expires = field(json.get(), "expires");
  if (!tokenSafe(access) || !tokenSafe(refresh) || !cJSON_IsNumber(expires)) return;
  auth[provider].access = access; auth[provider].refresh = refresh;
  auth[provider].expires = static_cast<int64_t>(expires->valuedouble);
  Serial.printf("Usage %s: stored credentials restored\n", providerNames[provider]);
}

bool authorize(unsigned provider, std::string& error, uint32_t& retryAfter) {
  // If saving a rotated token failed, retry the write before using it again.
  if (auth[provider].saveFailed && !saveAuth(provider)) { error = "auth_save_failed"; return false; }
  const int64_t now = time(nullptr);
  if (!auth[provider].access.isEmpty() && auth[provider].expires > now + 300) return true;
  std::string body;
  const int code = request(provider, true, body, retryAfter);
  if (code != 200) { error = "auth_http_" + std::to_string(code); return false; }
  Json json(cJSON_Parse(body.c_str()), cJSON_Delete);
  const char* access = str(json.get(), "access_token");
  const char* refresh = str(json.get(), "refresh_token");
  const auto* expires = field(json.get(), "expires_in");
  if (!tokenSafe(access) || (*refresh && !tokenSafe(refresh))) { error = "invalid_auth_response"; return false; }
  int64_t lifetime = 3600;
  if (cJSON_IsNumber(expires) && expires->valuedouble >= 60 && expires->valuedouble <= 2592000) lifetime = expires->valuedouble;
  auth[provider].access = access;
  if (*refresh) auth[provider].refresh = refresh;
  auth[provider].expires = now + lifetime;
  if (!saveAuth(provider)) { error = "auth_save_failed"; return false; }
  Serial.printf("Usage %s: refreshed credentials saved\n", providerNames[provider]);
  return true;
}

bool percentage(const cJSON* item, const char* name, usage::Window& window) {
  const auto* number = field(item, name);
  if (!cJSON_IsNumber(number) || !std::isfinite(number->valuedouble) || number->valuedouble < 0 || number->valuedouble > 1000) return false;
  window.available = true; window.usedPercent = number->valuedouble; return true;
}
bool claudeWindow(const cJSON* item, const char* percentKey, usage::Window& window) {
  if (!item || cJSON_IsNull(item)) return true;
  if (!percentage(item, percentKey, window)) return false;
  const auto* reset = field(item, "resets_at");
  if (!reset || cJSON_IsNull(reset)) return true;
  if (!cJSON_IsString(reset) || !calendar::parseDateTime(reset->valuestring, window.resetsAt)) return false;
  return true;
}
bool codexWindows(const cJSON* limits, usage::Window& session, usage::Window& weekly) {
  if (!cJSON_IsObject(limits)) return false;
  for (const char* key : {"primary_window", "secondary_window"}) {
    const auto* item = field(limits, key);
    if (!item || cJSON_IsNull(item)) continue;
    usage::Window window;
    const auto* duration = field(item, "limit_window_seconds");
    if (!cJSON_IsNumber(duration) || !percentage(item, "used_percent", window)) return false;
    const auto* reset = field(item, "reset_at");
    if (reset && !cJSON_IsNull(reset)) {
      if (!cJSON_IsNumber(reset) || reset->valuedouble < 1704067200 || reset->valuedouble > 4102444800.0) return false;
      window.resetsAt = reset->valuedouble;
    }
    if (duration->valuedouble == 604800) weekly = window;
    else if (duration->valuedouble > 0 && duration->valuedouble <= 86400) session = window;
    else return false;  // Do not label an unknown quota period as weekly/session.
  }
  return true;
}
bool modelName(const char* name) {
  return *name && strlen(name) <= 64 && message_text::valid(name) && !strpbrk(name, "\r\n\t");
}
bool parseUsage(unsigned provider, const std::string& body, usage::Provider& result) {
  Json json(cJSON_Parse(body.c_str()), cJSON_Delete);
  if (!cJSON_IsObject(json.get())) return false;
  usage::Provider next;
  next.configured = true;
  if (provider == 0) {
    if (!codexWindows(field(json.get(), "rate_limit"), next.session, next.weekly)) return false;
    const auto* extras = field(json.get(), "additional_rate_limits");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, extras) {
      const char* name = str(item, "limit_name");
      usage::Window session, weekly;
      if (next.models.size() < 4 && modelName(name) && codexWindows(field(item, "rate_limit"), session, weekly) && weekly.available) {
        usage::Provider::ModelLimit model; model.name = name; model.window = weekly; next.models.push_back(model);
      }
    }
  } else {
    if (!field(json.get(), "five_hour") || !field(json.get(), "seven_day") ||
        !claudeWindow(field(json.get(), "five_hour"), "utilization", next.session) ||
        !claudeWindow(field(json.get(), "seven_day"), "utilization", next.weekly)) return false;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, field(json.get(), "limits")) {
      if (strcmp(str(item, "kind"), "weekly_scoped")) continue;
      const auto* scope = field(item, "scope");
      const char* name = str(field(scope, "model"), "display_name");
      if (!modelName(name) || next.models.size() >= 4) continue;
      usage::Provider::ModelLimit model; model.name = name;
      if (!claudeWindow(item, "percent", model.window)) return false;
      next.models.push_back(model);
    }
  }
  next.fetchedAt = time(nullptr);
  result = std::move(next);
  return true;
}

bool fetch(unsigned provider, usage::Provider& state, uint32_t& retryAfter) {
  if (!authorize(provider, state.error, retryAfter)) return false;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    std::string body;
    const int code = request(provider, false, body, retryAfter);
    if (code == 401 && attempt == 0) {
      auth[provider].expires = 0;
      if (!authorize(provider, state.error, retryAfter)) return false;
      continue;
    }
    if (code != 200) { state.error = "usage_http_" + std::to_string(code); return false; }
    if (!parseUsage(provider, body, state)) { state.error = "invalid_usage_response"; return false; }
    return true;
  }
  return false;
}
void worker(void*) {
  usage::Snapshot state;
  usage::Provider* providers[] = {&state.codex, &state.claude};
  uint32_t last[2] = {0, 0}, interval[2] = {0, 0};
  bool attempted[2] = {false, false};
  for (unsigned p = 0; p < 2; ++p) { providers[p]->configured = *config::kUsageCredentials[p].refreshToken; loadAuth(p); }
  for (;;) {
    if (clockReady.load() && WiFi.status() == WL_CONNECTED) for (unsigned p = 0; p < 2; ++p) {
      auto& value = *providers[p];
      if (!value.configured || (attempted[p] && millis() - last[p] < interval[p])) continue;
      attempted[p] = true;
      uint32_t retryAfter = 0;
      work_gate::lock();
      const bool success = fetch(p, value, retryAfter);
      work_gate::unlock();
      last[p] = millis();
      interval[p] = success ? config::kUsageRefreshSeconds * 1000UL : std::max<uint32_t>(60, retryAfter) * 1000UL;
      if (!success && retryAfter == 0) interval[p] = 60000;
      Serial.printf("Usage %s: %s\n", providerNames[p], success ? "updated" : value.error.c_str());
      xSemaphoreTake(deliveryMutex, portMAX_DELAY);
      delivered = state; hasDelivery = true;
      xSemaphoreGive(deliveryMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

cJSON* windowJson(const usage::Window& window) {
  if (!window.available) return cJSON_CreateNull();
  auto* json = cJSON_CreateObject();
  cJSON_AddNumberToObject(json, "used_percent", window.usedPercent);
  if (window.resetsAt) cJSON_AddNumberToObject(json, "resets_at", window.resetsAt);
  else cJSON_AddNullToObject(json, "resets_at");
  return json;
}
cJSON* providerJson(const usage::Provider& value) {
  auto* json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "configured", value.configured);
  cJSON_AddNumberToObject(json, "fetched_at", value.fetchedAt);
  cJSON_AddBoolToObject(json, "stale", usage::stale(value, time(nullptr)));
  cJSON_AddStringToObject(json, "error", value.error.c_str());
  cJSON_AddItemToObject(json, "session", windowJson(value.session));
  cJSON_AddItemToObject(json, "weekly", windowJson(value.weekly));
  auto* models = cJSON_AddArrayToObject(json, "models");
  for (const auto& model : value.models) {
    auto* entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", model.name.c_str());
    cJSON_AddItemToObject(entry, "weekly", windowJson(model.window));
    cJSON_AddItemToArray(models, entry);
  }
  return json;
}
}  // namespace

namespace usage_service {
void begin(WebServer& server) {
  current.codex.configured = *config::kUsageCredentials[0].refreshToken;
  current.claude.configured = *config::kUsageCredentials[1].refreshToken;
  if (current.codex.configured || current.claude.configured) {
    storageReady = authStore.begin("paper-usage", false);
    deliveryMutex = xSemaphoreCreateMutex();
    if (!storageReady || !deliveryMutex || !work_gate::begin() || xTaskCreate(worker, "usage", 16384, nullptr, 1, nullptr) != pdPASS) {
      current.codex.error = current.claude.error = "usage_start_failed";
    }
  }
  server.on("/api/usage", HTTP_GET, [&server]() {
    const String token(config::apiToken("device"));
    if (!token.isEmpty() && server.header("Authorization") != "Bearer " + token) {
      server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return;
    }
    Json json(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddItemToObject(json.get(), "codex", providerJson(current.codex));
    cJSON_AddItemToObject(json.get(), "claude", providerJson(current.claude));
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", serialize(json.get()));
  });
}
void tick() {
  clockReady.store(calendar_service::snapshot().clockReady);
  if (deliveryMutex && xSemaphoreTake(deliveryMutex, 0) == pdTRUE) {
    if (hasDelivery) { current = delivered; hasDelivery = false; redraw = true; }
    xSemaphoreGive(deliveryMutex);
  }
  // Repaint only when a reset/staleness label changes, not every second.
  static std::string previous;
  const int64_t now = time(nullptr);
  std::string labels;
  for (const auto* provider : {&current.codex, &current.claude}) {
    for (const auto* window : {&provider->session, &provider->weekly}) labels += usage::resetLabel(*provider, *window, now);
    for (const auto& model : provider->models) labels += usage::resetLabel(*provider, model.window, now);
  }
  if (labels != previous) { previous = labels; redraw = true; }
}
bool pending() { return redraw; }
void rendered() { redraw = false; }
const usage::Snapshot& snapshot() { return current; }
}
