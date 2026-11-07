#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "skiplist.h"
#include "test_support.h"

namespace {

void TestVersionsAndReplacement() {
  kv::SkipList table;
  table.Put("key", 3, "new");
  table.Put("key", 1, "old");
  table.Delete("key", 2);
  CHECK(!table.Get("key", 0).has_value());
  CHECK(table.Get("key", 1)->value == "old");
  CHECK(table.Get("key", 2)->deleted);
  CHECK(table.Get("key", 3)->value == "new");
  CHECK(!table.Get("missing", 3).has_value());

  // An identical internal key keeps overwrite semantics without mutating a
  // payload that a concurrent reader may already be copying.
  table.Put("key", 3, "replacement");
  CHECK(table.Get("key", 3)->value == "replacement");
  table.Delete("key", 3);
  CHECK(table.Get("key", 3)->deleted);
  table.Put("key", 3, "restored");
  CHECK(table.Size() == 3);
  const auto entries = table.Entries();
  CHECK(entries.size() == 3);
  CHECK(entries[0].sequence == 3 && entries[0].value == "restored");
  CHECK(entries[1].sequence == 2 && entries[1].deleted);
  CHECK(entries[2].sequence == 1 && entries[2].value == "old");
}

constexpr size_t kKeys = 32;

void CheckVersion(const kv::Entry& entry, size_t round, size_t key) {
  CHECK(entry.sequence == round * kKeys + key + 1);
  CHECK(entry.deleted == (round != 0 && (round + key) % 7 == 0));
  CHECK(entry.value == (entry.deleted ? "" : "value_" + std::to_string(round)));
}

void CheckView(const kv::SkipList& table, size_t boundary) {
  const size_t round = boundary / kKeys - 1;
  for (size_t key = 0; key < kKeys; ++key) {
    const auto entry = table.Get(Key(key), boundary);
    CHECK(entry.has_value());
    CheckVersion(*entry, round, key);
    // The original snapshot must survive all later inserts and tombstones.
    const auto original = table.Get(Key(key), kKeys);
    CHECK(original.has_value());
    CheckVersion(*original, 0, key);
  }

  std::map<std::string, kv::Entry> visible;
  const auto entries = table.Entries();
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if (i != 0) {
      const auto& previous = entries[i - 1];
      CHECK(previous.key < entry.key ||
            (previous.key == entry.key && previous.sequence > entry.sequence));
    }
    if (entry.sequence <= boundary) {
      visible.emplace(entry.key, kv::Entry{entry.sequence, entry.value, entry.deleted});
    }
  }
  CHECK(visible.size() == kKeys);
  for (size_t key = 0; key < kKeys; ++key) CheckVersion(visible.at(Key(key)), round, key);
  CHECK(table.Size() >= boundary);
}

void TestConcurrentPublication() {
  kv::SkipList table;
  for (size_t key = 0; key < kKeys; ++key) table.Put(Key(key), key + 1, "value_0");
  std::atomic<size_t> boundary{kKeys};
  std::atomic<bool> halfway{false}, done{false};
  std::atomic<int> checked_half_batch{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      while (!halfway.load(std::memory_order_acquire)) std::this_thread::yield();
      CheckView(table, boundary.load(std::memory_order_acquire));
      checked_half_batch.fetch_add(1, std::memory_order_release);
      while (!done.load(std::memory_order_acquire)) {
        CheckView(table, boundary.load(std::memory_order_acquire));
      }
    });
  }
  constexpr size_t rounds = 128;
  for (size_t round = 1; round <= rounds; ++round) {
    for (size_t key = 0; key < kKeys; ++key) {
      const auto sequence = round * kKeys + key + 1;
      if ((round + key) % 7 == 0) table.Delete(Key(key), sequence);
      else table.Put(Key(key), sequence, "value_" + std::to_string(round));
      if (round == 1 && key == kKeys / 2) {
        // Hold a partially installed batch until every reader has completed an
        // old view. Pointer publication alone must not advance visibility.
        halfway.store(true, std::memory_order_release);
        while (checked_half_batch.load(std::memory_order_acquire) != 3)
          std::this_thread::yield();
      }
    }
    boundary.store((round + 1) * kKeys, std::memory_order_release);
  }
  done.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();
  CheckView(table, boundary.load(std::memory_order_acquire));
  CHECK(table.Size() == (rounds + 1) * kKeys);
}

void CheckReplacementPayload(const kv::Entry& entry) {
  if (entry.deleted) {
    CHECK(entry.value.empty());
  } else {
    CHECK(entry.value.size() == 512);
    CHECK(entry.value.find_first_not_of(entry.value.front()) == std::string::npos);
  }
}

void TestConcurrentReplacement() {
  kv::SkipList table;
  for (size_t key = 0; key < kKeys; ++key) table.Put(Key(key), 1, std::string(512, 'a'));
  std::atomic<bool> start{false}, done{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 3; ++i) {
    readers.emplace_back([&] {
      start.store(true, std::memory_order_release);
      do {
        for (size_t key = 0; key < kKeys; ++key) {
          const auto entry = table.Get(Key(key), 1);
          CHECK(entry.has_value() && entry->sequence == 1);
          CheckReplacementPayload(*entry);
        }
        const auto entries = table.Entries();
        CHECK(entries.size() == kKeys && table.Size() == kKeys);
        for (const auto& entry : entries)
          CheckReplacementPayload({entry.sequence, entry.value, entry.deleted});
      } while (!done.load(std::memory_order_acquire));
    });
  }
  while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
  for (size_t i = 0; i < 4096; ++i) {
    if (i % 7 == 0) table.Delete(Key(i % kKeys), 1);
    else table.Put(Key(i % kKeys), 1, std::string(512, static_cast<char>('a' + i % 26)));
  }
  done.store(true, std::memory_order_release);
  for (auto& reader : readers) reader.join();
  CHECK(table.Size() == kKeys);
}

}  // namespace

int main() {
  TestVersionsAndReplacement();
  TestConcurrentPublication();
  TestConcurrentReplacement();
  std::cout << "skiplist_test passed\n";
}
