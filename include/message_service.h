#pragma once

#include <WebServer.h>
#include <string>

namespace message_service {
void begin(WebServer& server);
const std::string& text();
bool storageReady();
bool pending();
void rendered();
}  // namespace message_service
