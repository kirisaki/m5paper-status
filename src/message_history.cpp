#include "message_history.h"
#include "message_text.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace message_history {
namespace {
void put32(std::string& out, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) out += static_cast<char>(n >> (8 * i));
}
uint32_t get32(const std::string& data, size_t offset) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= uint32_t(uint8_t(data[offset + i])) << (8 * i);
  return value;
}
uint32_t crc(const char* data, size_t size) {
  uint32_t value = 0xffffffff;
  for (size_t i = 0; i < size; ++i) {
    value ^= static_cast<uint8_t>(data[i]);
    for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1)));
  }
  return ~value;
}
bool validCrc(const std::string& data) {
  return data.size() >= 4 && get32(data, data.size() - 4) == crc(data.data(), data.size() - 4);
}
Result readFile(const std::string& path, size_t maximum, std::string& data) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return errno == ENOENT ? Result::NOT_FOUND : Result::IO_ERROR;
  data.clear();
  char buffer[512];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    if (data.size() + count > maximum) { std::fclose(file); return Result::CORRUPT; }
    data.append(buffer, count);
  }
  const bool failed = std::ferror(file);
  std::fclose(file);
  return failed ? Result::IO_ERROR : Result::OK;
}
Result writeAtomic(const std::string& path, std::string data) {
  put32(data, crc(data.data(), data.size()));
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return Result::IO_ERROR;
  bool success = std::fwrite(data.data(), 1, data.size(), file) == data.size();
  if (std::fflush(file) != 0) success = false;
  if (fsync(fileno(file)) != 0) success = false;
  if (std::fclose(file) != 0) success = false;
  if (!success || std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return Result::IO_ERROR;
  }
  return Result::OK;
}
}  // namespace

Store::Store(std::string directory, unsigned capacity)
    : directory_(std::move(directory)), capacity_(capacity) {}

std::string Store::recordPath(uint32_t id) const {
  return directory_ + "/record-" + std::to_string(id) + ".bin";
}

Result Store::commit(const std::vector<uint32_t>& ids, uint32_t next, bool migrated) {
  std::string data = "PMI1";
  put32(data, next); put32(data, migrated ? 1 : 0); put32(data, ids.size());
  for (uint32_t id : ids) put32(data, id);
  const Result result = writeAtomic(directory_ + "/index", data);
  if (result == Result::OK) { ids_ = ids; next_ = next; migrated_ = migrated; }
  return result;
}

Result Store::begin(const std::string* legacy) {
  ready_ = false;
  ids_.clear(); next_ = 1; migrated_ = false;
  if (capacity_ < 1 || capacity_ > kMaximumEntries) return Result::CORRUPT;
  if (mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST) return Result::IO_ERROR;
  std::string data;
  Result result = readFile(directory_ + "/index", 20 + 4 * kMaximumEntries, data);
  if (result == Result::NOT_FOUND) {
    // Only a fresh directory (possibly with an interrupted initial index write)
    // may be initialized. Never silently replace a missing existing manifest.
    DIR* dir = opendir(directory_.c_str());
    if (!dir) return Result::IO_ERROR;
    bool empty = true;
    while (auto* entry = readdir(dir)) {
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") && strcmp(entry->d_name, "index.tmp")) empty = false;
    }
    closedir(dir);
    if (!empty) return Result::CORRUPT;
    result = commit({}, 1, false);
    if (result != Result::OK) return result;
  } else {
    if (result != Result::OK) return result;
    if (data.size() < 20 || data.compare(0, 4, "PMI1") || !validCrc(data)) return Result::CORRUPT;
    next_ = get32(data, 4);
    const uint32_t migrated = get32(data, 8), count = get32(data, 12);
    if (!next_ || migrated > 1 || count > kMaximumEntries || data.size() != 20 + 4 * count) return Result::CORRUPT;
    migrated_ = migrated;
    for (unsigned i = 0; i < count; ++i) {
      const uint32_t id = get32(data, 16 + 4 * i);
      if (!id || id >= next_ || (!ids_.empty() && id <= ids_.back())) return Result::CORRUPT;
      ids_.push_back(id);
      Entry entry;
      if (readRecord(id, entry) != Result::OK) return Result::CORRUPT;
    }
    if (!migrated_ && !ids_.empty()) return Result::CORRUPT;
  }
  ready_ = true;
  collectGarbage();
  if (!migrated_) {
    uint32_t ignored = 0;
    result = legacy ? append(*legacy, 0, ignored) : commit(ids_, next_, true);
  }
  if (result == Result::OK && ids_.size() > capacity_) {
    std::vector<uint32_t> kept(ids_.end() - capacity_, ids_.end());
    result = commit(kept, next_, true);
    if (result == Result::OK) collectGarbage();
  }
  ready_ = result == Result::OK;
  return result;
}

Result Store::readRecord(uint32_t id, Entry& entry) const {
  std::string data;
  const Result result = readFile(recordPath(id), message_text::kMaxBytes + 24, data);
  if (result != Result::OK) return result;
  if (data.size() < 24 || data.compare(0, 4, "PMR1") || !validCrc(data)
      || get32(data, 4) != id || get32(data, 16) != data.size() - 24) return Result::CORRUPT;
  const uint64_t timestamp = uint64_t(get32(data, 8)) | (uint64_t(get32(data, 12)) << 32);
  if (timestamp > INT64_MAX) return Result::CORRUPT;
  entry.id = id; entry.receivedAt = timestamp;
  entry.text = data.substr(20, data.size() - 24);
  return message_text::valid(entry.text) ? Result::OK : Result::CORRUPT;
}

Result Store::read(uint32_t id, Entry& entry) const {
  if (!ready_) return Result::IO_ERROR;
  if (!std::binary_search(ids_.begin(), ids_.end(), id)) return Result::NOT_FOUND;
  return readRecord(id, entry);
}

Result Store::append(const std::string& text, int64_t receivedAt, uint32_t& id) {
  if (!ready_) return Result::IO_ERROR;
  if (text.size() > message_text::kMaxBytes || !message_text::valid(text) || receivedAt < 0) return Result::CORRUPT;
  if (next_ == UINT32_MAX) return Result::ID_EXHAUSTED;
  collectGarbage();
  std::string data = "PMR1";
  put32(data, next_); put32(data, uint64_t(receivedAt)); put32(data, uint64_t(receivedAt) >> 32);
  put32(data, text.size()); data += text;
  Result result = writeAtomic(recordPath(next_), data);
  if (result != Result::OK) return result;
  std::vector<uint32_t> nextIds = ids_;
  nextIds.push_back(next_);
  if (nextIds.size() > capacity_) nextIds.erase(nextIds.begin());
  const uint32_t assigned = next_;
  result = commit(nextIds, next_ + 1, true);
  if (result == Result::OK) { id = assigned; collectGarbage(); }
  return result;
}

Result Store::erase(uint32_t id) {
  if (!ready_) return Result::IO_ERROR;
  std::vector<uint32_t> kept = ids_;
  const auto found = std::lower_bound(kept.begin(), kept.end(), id);
  if (found == kept.end() || *found != id) return Result::NOT_FOUND;
  kept.erase(found);
  const Result result = commit(kept, next_, true);
  if (result == Result::OK) collectGarbage();
  return result;
}

void Store::collectGarbage() const {
  DIR* dir = opendir(directory_.c_str());
  if (!dir) return;
  std::vector<std::string> remove;
  while (auto* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.compare(0, 7, "record-") != 0) continue;
    const auto dot = name.find('.', 7);
    if (dot == std::string::npos) continue;
    const std::string number = name.substr(7, dot - 7), suffix = name.substr(dot);
    if (number.empty() || number.size() > 10 || number.find_first_not_of("0123456789") != std::string::npos) continue;
    uint64_t id = 0;
    for (char c : number) id = id * 10 + (c - '0');
    if (id > UINT32_MAX || (suffix != ".bin" && suffix != ".bin.tmp")) continue;
    if (suffix == ".bin.tmp" || !std::binary_search(ids_.begin(), ids_.end(), uint32_t(id))) remove.push_back(directory_ + "/" + name);
  }
  closedir(dir);
  for (const auto& path : remove) std::remove(path.c_str());
}
}  // namespace message_history
