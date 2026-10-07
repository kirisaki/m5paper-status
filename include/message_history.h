#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace message_history {
constexpr unsigned kMaximumEntries = 300;
enum class Result { OK, NOT_FOUND, IO_ERROR, CORRUPT, ID_EXHAUSTED };
struct Entry {
  uint32_t id = 0;
  int64_t receivedAt = 0;  // Zero means the clock had not synchronized.
  std::string text;
};

// Records are immutable. An atomic manifest replacement commits both POST and
// DELETE; unreferenced files can then be reclaimed safely after a power loss.
class Store {
 public:
  Store(std::string directory, unsigned capacity);
  Result begin(const std::string* legacy = nullptr);
  Result append(const std::string& text, int64_t receivedAt, uint32_t& id);
  Result read(uint32_t id, Entry& entry) const;
  Result erase(uint32_t id);
  const std::vector<uint32_t>& ids() const { return ids_; }
  unsigned capacity() const { return capacity_; }
 private:
  std::string directory_;
  unsigned capacity_;
  uint32_t next_ = 1;
  bool migrated_ = false;
  bool ready_ = false;
  std::vector<uint32_t> ids_;
  std::string recordPath(uint32_t id) const;
  Result readRecord(uint32_t id, Entry& entry) const;
  Result commit(const std::vector<uint32_t>& ids, uint32_t next, bool migrated);
  void collectGarbage() const;
};
}  // namespace message_history
