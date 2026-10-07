#pragma once
#include <WebServer.h>
#include "usage_model.h"
namespace usage_service {
void begin(WebServer& server);
void tick();
bool pending();
void rendered();
const usage::Snapshot& snapshot();
}
