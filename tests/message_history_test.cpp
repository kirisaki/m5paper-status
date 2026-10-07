#include "message_history.h"
#include <cassert>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using message_history::Store;
using message_history::Entry;
using message_history::Result;

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  const std::string directory = root + "/history";
  const std::string legacy = "以前の本文\n日本語";
  Store store(directory, 3);
  assert(store.begin(&legacy) == Result::OK);
  Entry entry;
  assert(store.ids().size() == 1 && store.read(1, entry) == Result::OK);
  assert(entry.text == legacy && entry.receivedAt == 0);
  uint32_t id = 0;
  assert(store.append("second", 1791400000, id) == Result::OK && id == 2);
  assert(store.append(std::string(4096, 'a'), 1791400001, id) == Result::OK && id == 3);
  assert(store.append("fourth", 1791400002, id) == Result::OK && id == 4);
  assert(store.ids() == std::vector<uint32_t>({2, 3, 4}));
  assert(store.read(1, entry) == Result::NOT_FOUND);
  assert(access((directory + "/record-1.bin").c_str(), F_OK) != 0);
  assert(store.append(std::string(4097, 'a'), 0, id) == Result::CORRUPT);
  assert(store.read(4, entry) == Result::OK && entry.receivedAt == 1791400002);

  // Interrupted manifest writes leave the previous history unchanged. The new
  // record is an orphan and must not be visible after reboot.
  assert(mkdir((directory + "/index.tmp").c_str(), 0700) == 0);
  assert(store.append("not committed", 0, id) == Result::IO_ERROR);
  assert(store.ids() == std::vector<uint32_t>({2, 3, 4}));
  assert(store.erase(4) == Result::IO_ERROR);
  assert(rmdir((directory + "/index.tmp").c_str()) == 0);
  Store reboot(directory, 3);
  assert(reboot.begin(&legacy) == Result::OK);
  assert(reboot.ids() == std::vector<uint32_t>({2, 3, 4}));
  assert(access((directory + "/record-5.bin").c_str(), F_OK) != 0);
  assert(reboot.erase(3) == Result::OK);
  assert(reboot.erase(3) == Result::NOT_FOUND);
  assert(reboot.erase(4) == Result::OK);
  assert(reboot.read(reboot.ids().back(), entry) == Result::OK && entry.text == "second");
  assert(reboot.erase(2) == Result::OK && reboot.ids().empty());
  Store empty(directory, 3);
  assert(empty.begin(&legacy) == Result::OK && empty.ids().empty());
  assert(empty.append("", 0, id) == Result::OK && id == 5);
  assert(empty.append("new", 42, id) == Result::OK && id == 6);
  Store smaller(directory, 1);
  assert(smaller.begin() == Result::OK && smaller.ids() == std::vector<uint32_t>({6}));

  // Corrupt records or index files are reported; they are never reformatted.
  { std::ofstream file(directory + "/record-6.bin", std::ios::app); file << 'x'; }
  Store corrupted(directory, 3);
  assert(corrupted.begin() == Result::CORRUPT);
  const std::string other = root + "/other";
  Store clean(other, 1);
  assert(clean.begin() == Result::OK);
  { std::ofstream file(other + "/index", std::ios::trunc); file << "bad"; }
  Store badIndex(other, 1);
  assert(badIndex.begin() == Result::CORRUPT);

  // Worst-case retention: 300 full UTF-8-safe payloads and their atomic index.
  Store full(root + "/full", 300);
  assert(full.begin() == Result::OK);
  for (unsigned i = 0; i < 302; ++i) assert(full.append(std::string(4096, 'x'), i, id) == Result::OK);
  assert(full.ids().size() == 300 && full.ids().front() == 3 && full.ids().back() == 302);
  Store fullReboot(root + "/full", 300);
  assert(fullReboot.begin() == Result::OK && fullReboot.ids() == full.ids());
}
