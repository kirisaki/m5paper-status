#pragma once
namespace work_gate {
bool begin();
void lock();
bool tryLock();
void unlock();
}
