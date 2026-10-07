#include "message_service.h"

#include <Preferences.h>
#include <LittleFS.h>
#include <esp_partition.h>
#include <cJSON.h>
#include <algorithm>
#include <ctime>
#include <memory>

#include "config.h"
#include "message_text.h"
#include "message_history.h"
#include "calendar_service.h"

namespace {
Preferences preferences;
message_history::Store history("/littlefs/messages", config::kMessageHistoryLimit);
using Result = message_history::Result;
bool storageAvailable = false;
bool refreshPending = true;
std::string latest;
int64_t latestReceivedAt = 0;
uint32_t revision = 1;
std::string incoming;
bool complete = false;
bool rejected = false;
size_t expectedBytes = 0;

String latestId() {
  return history.ids().empty() ? String("null") : String(history.ids().back());
}

bool mountHistory() {
  if (LittleFS.begin(false)) return true;
  // Only an entirely erased partition may be formatted automatically. A mount
  // failure on an existing filesystem must not destroy message history.
  const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition) return false;
  uint8_t block[512];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
    const size_t size = std::min(sizeof(block), partition->size - offset);
    if (esp_partition_read(partition, offset, block, size) != ESP_OK) return false;
    for (size_t i = 0; i < size; ++i) if (block[i] != 0xff) return false;
    if (offset % 65536 == 0) yield();
  }
  Serial.println("Formatting unused message history partition...");
  return LittleFS.begin(true);
}

void error(WebServer& server, int status, const char* reason, bool close = false) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json", String("{\"error\":\"") + reason + "\"}");
  if (close) server.client().stop();
}

bool authorized(WebServer& server) {
  const char* token = config::apiToken("device");
  if (token[0] == '\0' || server.header("Authorization") == String("Bearer ") + token) {
    return true;
  }
  server.sendHeader("WWW-Authenticate", "Bearer");
  error(server, 401, "unauthorized");
  return false;
}

bool plainText(WebServer& server) {
  String contentType = server.header("Content-Type");
  contentType.toLowerCase();
  const int separator = contentType.indexOf(';');
  String media = separator < 0 ? contentType : contentType.substring(0, separator);
  media.trim();
  if (media != "text/plain") return false;
  const int charset = contentType.indexOf("charset=");
  if (charset >= 0) {
    String encoding = contentType.substring(charset + 8);
    const int end = encoding.indexOf(';');
    if (end >= 0) encoding = encoding.substring(0, end);
    encoding.trim();
    encoding.replace("\"", "");
    if (encoding != "utf-8") return false;
  }
  return true;
}

class MessageHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override { return uri == "/api/message"; }
  bool canRaw(String uri) override { return uri == "/api/message"; }

  void raw(WebServer& server, String uri, HTTPRaw& raw) override {
    if (raw.status == RAW_START) {
      incoming.clear();
      complete = false;
      rejected = false;
      expectedBytes = 0;
      if (server.method() != HTTP_POST) {
        rejected = true;
        error(server, 405, "method_not_allowed", true);
        return;
      }
      if (!authorized(server)) {
        rejected = true;
        server.client().stop();
        return;
      }
      if (!plainText(server)) {
        rejected = true;
        error(server, 415, "use_text_plain_utf8", true);
        return;
      }
      const String length = server.header("Content-Length");
      if (length.isEmpty() || !server.header("Transfer-Encoding").isEmpty()) {
        rejected = true;
        error(server, 411, "content_length_required", true);
        return;
      }
      for (size_t i = 0; i < length.length(); ++i) {
        if (length[i] < '0' || length[i] > '9') {
          rejected = true;
          error(server, 400, "invalid_content_length", true);
          return;
        }
        expectedBytes = expectedBytes * 10 + (length[i] - '0');
        if (expectedBytes > message_text::kMaxBytes) {
          rejected = true;
          error(server, 413, "message_too_large", true);
          return;
        }
      }
      incoming.reserve(expectedBytes);
      server.client().setTimeout(1000);
    } else if (raw.status == RAW_WRITE && !rejected) {
      if (raw.currentSize > expectedBytes - incoming.size()) {
        rejected = true;
        error(server, 400, "invalid_body_length", true);
        return;
      }
      incoming.append(reinterpret_cast<const char*>(raw.buf), raw.currentSize);
    } else if (raw.status == RAW_END && !rejected) {
      complete = incoming.size() == expectedBytes;
    } else if (raw.status == RAW_ABORTED) {
      complete = false;
      incoming.clear();
    }
  }

  bool handle(WebServer& server, HTTPMethod method, String uri) override {
    if (method != HTTP_GET && method != HTTP_POST) {
      server.sendHeader("Allow", "GET, POST");
      error(server, 405, "method_not_allowed");
      return true;
    }
    if (!authorized(server)) return true;
    if (method == HTTP_GET) {
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "text/plain; charset=utf-8", latest.c_str());
      return true;
    }
    if (!plainText(server)) {
      error(server, 415, "use_text_plain_utf8");
      return true;
    }
    if (rejected) return true;
    if (!complete || !message_text::valid(incoming)) {
      error(server, 400, "invalid_utf8_or_control_character");
      return true;
    }
    if (!storageAvailable) {
      error(server, 503, "storage_unavailable");
      return true;
    }
    const bool changed = incoming != latest;
    if (changed) {
      uint32_t id = 0;
      const int64_t timestamp = calendar_service::snapshot().clockReady ? time(nullptr) : 0;
      if (history.append(incoming, timestamp, id) != Result::OK) {
        error(server, 500, "save_failed");
        return true;
      }
      latest = incoming;
      latestReceivedAt = timestamp;
      ++revision;
      refreshPending = true;
      Serial.printf("Message saved: %u bytes\n", static_cast<unsigned>(latest.size()));
    }
    server.sendHeader("Cache-Control", "no-store");
    const String response = String("{\"status\":\"") + (changed ? "accepted" : "unchanged") +
                            "\",\"bytes\":" + String(latest.size()) + ",\"id\":" + latestId() + "}";
    server.send(changed ? 202 : 200, "application/json", response);
    return true;
  }
};

bool positiveInteger(const String& value, uint32_t& result) {
  if (value.isEmpty() || value.length() > 10) return false;
  uint64_t number = 0;
  for (unsigned i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    number = number * 10 + value[i] - '0';
    if (number > UINT32_MAX) return false;
  }
  result = number;
  return result != 0;
}

String entryJson(const message_history::Entry& entry) {
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> object(cJSON_CreateObject(), cJSON_Delete);
  if (!object) return "";
  if (!cJSON_AddNumberToObject(object.get(), "id", entry.id)
      || !(entry.receivedAt ? cJSON_AddNumberToObject(object.get(), "received_at", entry.receivedAt)
                           : cJSON_AddNullToObject(object.get(), "received_at"))
      || !cJSON_AddNumberToObject(object.get(), "bytes", entry.text.size())
      || !cJSON_AddStringToObject(object.get(), "text", entry.text.c_str())) return "";
  char* encoded = cJSON_PrintUnformatted(object.get());
  const String result(encoded ? encoded : "");
  cJSON_free(encoded);
  return result;
}

class HistoryHandler : public RequestHandler {
  bool rawRejected = false;
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    const bool matches = uri == "/api/messages" || uri.startsWith("/api/messages/");
    if (matches) rawRejected = false;
    return matches;
  }
  bool canRaw(String uri) override { return canHandle(HTTP_ANY, uri); }
  void raw(WebServer& server, String uri, HTTPRaw& raw) override {
    if (raw.status != RAW_START) return;
    rawRejected = true;
    if (!authorized(server)) { server.client().stop(); return; }
    if (server.method() != HTTP_DELETE || uri == "/api/messages") {
      server.sendHeader("Allow", uri == "/api/messages" ? "GET" : "GET, DELETE");
      error(server, 405, "method_not_allowed", true); return;
    }
    const String length = server.header("Content-Length");
    if ((!length.isEmpty() && length != "0") || !server.header("Transfer-Encoding").isEmpty()) {
      error(server, 400, "body_not_allowed", true); return;
    }
    rawRejected = false;
  }
  bool handle(WebServer& server, HTTPMethod method, String uri) override {
    if (rawRejected) return true;
    if (!authorized(server)) return true;
    const bool listing = uri == "/api/messages";
    if (method != HTTP_GET && (listing || method != HTTP_DELETE)) {
      server.sendHeader("Allow", listing ? "GET" : "GET, DELETE");
      error(server, 405, "method_not_allowed"); return true;
    }
    if (method == HTTP_DELETE) {
      const String length = server.header("Content-Length");
      if ((!length.isEmpty() && length != "0") || !server.header("Transfer-Encoding").isEmpty()) {
        error(server, 400, "body_not_allowed", true); return true;
      }
    }
    if (!storageAvailable) { error(server, 503, "storage_unavailable"); return true; }
    server.sendHeader("Cache-Control", "no-store");
    if (listing) {
      uint32_t limit = 20, before = UINT32_MAX;
      if ((server.hasArg("limit") && (!positiveInteger(server.arg("limit"), limit) || limit > 50))
          || (server.hasArg("before") && !positiveInteger(server.arg("before"), before))) {
        error(server, 400, "invalid_pagination"); return true;
      }
      std::vector<uint32_t> page;
      bool more = false;
      for (auto it = history.ids().rbegin(); it != history.ids().rend(); ++it) {
        if (*it >= before) continue;
        if (page.size() == limit) { more = true; break; }
        page.push_back(*it);
      }
      // Validate before sending HTTP headers, then stream one record at a time
      // so a page of 50 full messages does not occupy hundreds of KB of RAM.
      message_history::Entry entry;
      for (uint32_t id : page) if (history.read(id, entry) != Result::OK) {
        error(server, 500, "read_failed"); return true;
      }
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.send(200, "application/json; charset=utf-8", "");
      server.sendContent("{\"messages\":[");
      bool first = true;
      for (uint32_t id : page) {
        if (history.read(id, entry) != Result::OK) { server.client().stop(); return true; }
        const String json = entryJson(entry);
        if (json.isEmpty()) { server.client().stop(); return true; }
        if (!first) server.sendContent(",");
        server.sendContent(json); first = false;
      }
      server.sendContent(String("],\"next_before\":") + (more ? String(page.back()) : String("null"))
                         + ",\"total\":" + String(history.ids().size())
                         + ",\"capacity\":" + String(history.capacity()) + "}");
      server.sendContent("");
      return true;
    }
    uint32_t id = 0;
    if (!positiveInteger(uri.substring(strlen("/api/messages/")), id)) {
      error(server, 400, "invalid_message_id"); return true;
    }
    message_history::Entry entry;
    const Result found = history.read(id, entry);
    if (found != Result::OK) {
      error(server, found == Result::NOT_FOUND ? 404 : 500, found == Result::NOT_FOUND ? "not_found" : "read_failed");
      return true;
    }
    if (method == HTTP_GET) {
      const String json = entryJson(entry);
      if (json.isEmpty()) error(server, 500, "serialization_failed");
      else server.send(200, "application/json; charset=utf-8", json);
      return true;
    }
    const bool deletingLatest = history.ids().back() == id;
    std::string replacement;
    int64_t replacementReceivedAt = 0;
    if (deletingLatest && history.ids().size() > 1) {
      if (history.read(history.ids()[history.ids().size() - 2], entry) != Result::OK) {
        error(server, 500, "read_failed"); return true;
      }
      replacement = std::move(entry.text);
      replacementReceivedAt = entry.receivedAt;
    }
    if (history.erase(id) != Result::OK) { error(server, 500, "delete_failed"); return true; }
    ++revision;
    if (deletingLatest) {
      latest = std::move(replacement);
      latestReceivedAt = replacementReceivedAt;
      refreshPending = true;
    }
    server.send(200, "application/json", String("{\"status\":\"deleted\",\"id\":") + String(id)
                + ",\"latest_id\":" + latestId() + "}");
    return true;
  }
};
}  // namespace

namespace message_service {
void begin(WebServer& server) {
  const bool legacyAvailable = preferences.begin("paper-message", false);
  bool hasLegacy = false;
  if (legacyAvailable && preferences.isKey("message")) {
    const size_t size = preferences.getBytesLength("message");
    if (size > 0 && size <= message_text::kMaxBytes + 1) {
      std::string stored(size, '\0');
      if (preferences.getBytes("message", &stored[0], size) == size && stored.back() == '\0') {
        stored.pop_back();
        if (message_text::valid(stored)) { latest = stored; hasLegacy = true; }
      }
    }
  }
  storageAvailable = mountHistory() && history.begin(hasLegacy ? &latest : nullptr) == Result::OK;
  if (storageAvailable) {
    if (history.ids().empty()) latest.clear();
    else {
      message_history::Entry entry;
      storageAvailable = history.read(history.ids().back(), entry) == Result::OK;
      if (storageAvailable) {
        latest = std::move(entry.text);
        latestReceivedAt = entry.receivedAt;
      }
    }
    // Remove the old copy only after migration commits. An empty history must
    // never resurrect a deleted message from NVS on a later restart.
    if (storageAvailable && hasLegacy) preferences.remove("message");
  }
  if (legacyAvailable) preferences.end();
  Serial.printf("Message history: %s, %u entries, restored %u bytes\n",
                storageAvailable ? "ready" : "unavailable", static_cast<unsigned>(history.ids().size()),
                static_cast<unsigned>(latest.size()));
  const char* headers[] = {"Content-Type", "Content-Length", "Transfer-Encoding", "Authorization"};
  server.collectHeaders(headers, 4);
  server.addHandler(new MessageHandler());
  server.addHandler(new HistoryHandler());
}
const std::string& text() { return latest; }
int64_t receivedAt() { return latestReceivedAt; }
bool storageReady() { return storageAvailable; }
size_t historyCount() { return storageAvailable ? history.ids().size() : 0; }
uint32_t historyRevision() { return revision; }
const std::vector<uint32_t>& historyIds() {
  static const std::vector<uint32_t> empty;
  return storageAvailable ? history.ids() : empty;
}
bool readHistory(uint32_t id, message_history::Entry& entry) {
  return storageAvailable && history.read(id, entry) == Result::OK;
}
bool pending() { return refreshPending; }
void rendered() { refreshPending = false; }
}  // namespace message_service
