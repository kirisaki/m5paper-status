#pragma once

#include <WebServer.h>
#include "calendar_model.h"

namespace calendar_service {
void begin(WebServer& server);
// Only call tick/snapshot/rendered from the Arduino loop task.
void tick();
bool pending();
void rendered();
const calendar::Snapshot& snapshot();
// TLS and FreeType share the small internal heap; skip a render rather than
// blocking the HTTP server while the calendar worker is using TLS.
bool tryBeginRender();
void endRender();
}  // namespace calendar_service
