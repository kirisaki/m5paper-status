#include "message_service.h"

#include <Preferences.h>

#include "config.h"
#include "message_text.h"

namespace {
Preferences preferences;
bool storageAvailable = false;
bool refreshPending = true;
std::string latest;
std::string incoming;
bool complete = false;
bool rejected = false;
size_t expectedBytes = 0;

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
      // One NVS blob keeps the latest message atomic across power loss.
      const size_t size = incoming.size() + 1;
      if (preferences.putBytes("message", incoming.c_str(), size) != size) {
        error(server, 500, "save_failed");
        return true;
      }
      latest = incoming;
      refreshPending = true;
      Serial.printf("Message saved: %u bytes\n", static_cast<unsigned>(latest.size()));
    }
    server.sendHeader("Cache-Control", "no-store");
    const String response = String("{\"status\":\"") + (changed ? "accepted" : "unchanged") +
                            "\",\"bytes\":" + String(latest.size()) + "}";
    server.send(changed ? 202 : 200, "application/json", response);
    return true;
  }
};
}  // namespace

namespace message_service {
void begin(WebServer& server) {
  storageAvailable = preferences.begin("paper-message", false);
  if (storageAvailable && preferences.isKey("message")) {
    const size_t size = preferences.getBytesLength("message");
    if (size > 0 && size <= message_text::kMaxBytes + 1) {
      std::string stored(size, '\0');
      if (preferences.getBytes("message", &stored[0], size) == size && stored.back() == '\0') {
        stored.pop_back();
        if (message_text::valid(stored)) latest = stored;
      }
    }
  }
  Serial.printf("Message storage: %s, restored %u bytes\n",
                storageAvailable ? "ready" : "unavailable", static_cast<unsigned>(latest.size()));
  const char* headers[] = {"Content-Type", "Content-Length", "Transfer-Encoding", "Authorization"};
  server.collectHeaders(headers, 4);
  server.addHandler(new MessageHandler());
}
const std::string& text() { return latest; }
bool storageReady() { return storageAvailable; }
bool pending() { return refreshPending; }
void rendered() { refreshPending = false; }
}  // namespace message_service
