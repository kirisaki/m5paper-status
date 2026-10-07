#include "work_gate.h"
#include <Arduino.h>
namespace {
SemaphoreHandle_t gate = nullptr;
}
namespace work_gate {
bool begin() {
  if (!gate) gate = xSemaphoreCreateMutex();
  return gate != nullptr;
}
void lock() { if (gate) xSemaphoreTake(gate, portMAX_DELAY); }
bool tryLock() { return !gate || xSemaphoreTake(gate, 0) == pdTRUE; }
void unlock() { if (gate) xSemaphoreGive(gate); }
}
