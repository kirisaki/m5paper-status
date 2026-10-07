#pragma once

#include <Arduino.h>
#include <string>
#include "calendar_model.h"
#include "usage_model.h"

namespace message_display {
void begin();
void drawDashboard(const std::string& text, const calendar::Snapshot& calendar, const usage::Snapshot& usage);
void present();
bool sdReady();
bool truncated();
const char* bodyFont();
}  // namespace message_display
