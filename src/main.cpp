#include <Arduino.h>
#include <ESPmDNS.h>
#include <M5Unified.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "message_display.h"
#include "message_service.h"
#include "calendar_service.h"
#include "usage_service.h"
#include "history_ui.h"

namespace {
WebServer server(config::kHttpPort);
bool serverStarted = false;
bool mdnsStarted = false;
bool wasConnected = false;
uint32_t lastWifiAttempt = 0;
uint32_t lastMdnsAttempt = 0;
IPAddress displayedIp;
uint32_t lastMessageRefresh = 0;
history_ui::State ui;
uint32_t seenHistoryRevision = 0;

void updateTouch() {
  const unsigned count = M5.Touch.getCount();
  if (count != 1) { ui.cancelTouch(); return; }
  const auto& touch = M5.Touch.getDetail();
  if (touch.wasPressed()) ui.press(touch.x, touch.y);
  else if (touch.isPressed()) ui.move(touch.x, touch.y);
  else if (touch.wasReleased()) ui.release(touch.x, touch.y);
}

void drawScreen() {
  if (ui.screen() == history_ui::Screen::DASHBOARD) {
    message_display::drawDashboard(message_service::text(), message_service::receivedAt(), calendar_service::snapshot(), usage_service::snapshot());
    return;
  }
  std::vector<message_history::Entry> entries;
  bool readable = message_service::storageReady();
  const auto load = [&](uint32_t id) {
    message_history::Entry entry;
    if (message_service::readHistory(id, entry)) entries.push_back(std::move(entry));
    else readable = false;
  };
  if (ui.screen() == history_ui::Screen::DETAIL) load(ui.selected());
  else {
    const auto& ids = message_service::historyIds();
    for (size_t i = ui.offset(); i < ids.size() && i < ui.offset() + history_ui::kRows; ++i) load(ids[ids.size() - 1 - i]);
  }
  message_display::drawHistory(ui, entries, readable);
}

void configureApi() {
  server.on("/api/health", HTTP_GET, []() {
    String body = "{\"status\":\"ok\",\"device\":\"m5paper\",\"uptime_ms\":";
    body += String(millis());
    body += ",\"free_heap_bytes\":";
    body += String(ESP.getFreeHeap());
    body += ",\"hostname\":\"";
    body += config::kHostname;
    body += "\",\"sd_ready\":";
    body += message_display::sdReady() ? "true" : "false";
    body += ",\"storage_ready\":";
    body += message_service::storageReady() ? "true" : "false";
    body += ",\"message_bytes\":";
    body += String(message_service::text().size());
    body += ",\"message_history_count\":";
    body += String(message_service::historyCount());
    body += ",\"message_history_limit\":";
    body += String(config::kMessageHistoryLimit);
    body += ",\"screen\":\"";
    body += ui.screen() == history_ui::Screen::DASHBOARD ? "dashboard" :
            ui.screen() == history_ui::Screen::LIST ? "history" : "message";
    body += "\",\"history_offset\":";
    body += String(ui.offset());
    body += ",\"message_pending\":";
    body += message_service::pending() ? "true" : "false";
    body += ",\"message_truncated\":";
    body += message_display::truncated() ? "true" : "false";
    body += ",\"body_font\":\"";
    body += message_display::bodyFont();
    body += "\"";
    body += ",\"calendar_configured\":";
    body += calendar_service::snapshot().configured ? "true" : "false";
    body += ",\"calendar_ready\":";
    body += calendar_service::snapshot().ready ? "true" : "false";
    body += ",\"clock_ready\":";
    body += calendar_service::snapshot().clockReady ? "true" : "false";
    body += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", body);
  });
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"not_found\"}");
  });
}

void updateNetwork() {
  if (config::kWifiSsid[0] == '\0') {
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
    }
    const IPAddress ip = WiFi.localIP();
    bool redraw = !wasConnected || ip != displayedIp;
    if (redraw && mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }
    const uint32_t now = millis();
    if (!mdnsStarted && (redraw || now - lastMdnsAttempt >= config::kWifiRetryMs)) {
      lastMdnsAttempt = now;
      if (MDNS.begin(config::kHostname)) {
        mdnsStarted = MDNS.addService("http", "tcp", config::kHttpPort);
        if (!mdnsStarted) {
          MDNS.end();
        }
      }
      if (mdnsStarted) {
        Serial.printf("mDNS: http://%s.local/api/health\n", config::kHostname);
        redraw = true;
      } else {
        Serial.println("mDNS start failed; will retry.");
      }
    }
    if (redraw) {
      displayedIp = ip;
      Serial.printf("API: http://%s/api/health\n", ip.toString().c_str());
    }
    server.handleClient();
  } else {
    if (wasConnected) {
      if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
      }
      server.stop();
      serverStarted = false;
      Serial.println("Wi-Fi disconnected; retrying.");
    }
    const uint32_t now = millis();
    if (now - lastWifiAttempt >= config::kWifiRetryMs) {
      lastWifiAttempt = now;
      WiFi.reconnect();
    }
  }
  wasConnected = connected;
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.println("Starting M5Paper...");
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.fallback_board = m5::board_t::board_M5Paper;
  M5.begin(cfg);
  Serial.println("M5Paper initialized.");
  M5.Display.setRotation(3);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  message_display::begin();
  message_service::begin(server);
  calendar_service::begin(server);
  usage_service::begin(server);
  ui.sync(message_service::historyIds());
  seenHistoryRevision = message_service::historyRevision();
  drawScreen();
  message_display::present();
  message_service::rendered();
  calendar_service::rendered();
  usage_service::rendered();
  configureApi();

  if (config::kWifiSsid[0] == '\0') {
    Serial.println("Wi-Fi not configured. Set wifi in the build configuration JSON.");
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(config::kHostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(config::kWifiSsid, config::kWifiPassword);
  lastWifiAttempt = millis();
}

void loop() {
  M5.update();
  updateTouch();
  updateNetwork();
  calendar_service::tick();
  usage_service::tick();
  if (seenHistoryRevision != message_service::historyRevision()) {
    ui.sync(message_service::historyIds());
    seenHistoryRevision = message_service::historyRevision();
  }
  // Acknowledge and persist submissions before the slow EPD refresh.
  // Coalesce rapid posts and refresh at most once every two seconds.
  const bool dashboardPending = ui.screen() == history_ui::Screen::DASHBOARD &&
      (message_service::pending() || calendar_service::pending() || usage_service::pending());
  if ((ui.dirty() || dashboardPending) && !ui.touching() && millis() - lastMessageRefresh >= (ui.dirty() ? 100 : 2000) &&
      calendar_service::tryBeginRender()) {
    drawScreen();
    message_display::present();
    calendar_service::endRender();
    message_service::rendered();
    calendar_service::rendered();
    usage_service::rendered();
    ui.rendered();
    lastMessageRefresh = millis();
  }
  delay(10);
}
