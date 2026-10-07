#pragma once

#include <WebServer.h>
#include <string>
#include "message_history.h"

namespace message_service {
void begin(WebServer& server);
const std::string& text();
int64_t receivedAt();
bool storageReady();
size_t historyCount();
uint32_t historyRevision();
const std::vector<uint32_t>& historyIds();
bool readHistory(uint32_t id, message_history::Entry& entry);
bool pending();
void rendered();
}  // namespace message_service
