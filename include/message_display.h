#pragma once

#include <Arduino.h>
#include <string>
#include "calendar_model.h"
#include "usage_model.h"
#include "history_ui.h"
#include "message_history.h"

namespace message_display {
void begin();
void drawDashboard(const std::string& text, int64_t receivedAt, const calendar::Snapshot& calendar, const usage::Snapshot& usage);
void drawHistory(history_ui::State& state, const std::vector<message_history::Entry>& entries, bool readable);
void present();
bool sdReady();
bool truncated();
const char* bodyFont();
}  // namespace message_display
