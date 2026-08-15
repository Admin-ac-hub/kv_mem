#include "db.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "format.h"
#include "internal_iterator.h"

namespace kv {

namespace {

bool ContainsInvalidKeyByte(const std::string& key) {
  return key.find('\t') != std::string::npos ||
         key.find('\n') != std::string::npos ||
         key.find('\0') != std::string::npos;
}

Status ValidateKey(const std::string& key) {
  if (key.empty()) {
    return Status::InvalidArgument("key cannot be empty");
  }
  if (ContainsInvalidKeyByte(key)) {
    return Status::InvalidArgument("key cannot contain tab, newline, or NUL");
  }
  return Status::OK();
}

bool RangesOverlap(const SSTableMeta& lhs, const SSTableMeta& rhs) {
  if (lhs.smallest_key.empty() || lhs.largest_key.empty() ||
      rhs.smallest_key.empty() || rhs.largest_key.empty()) {
    return true;
  }
  return lhs.smallest_key <= rhs.largest_key && rhs.smallest_key <= lhs.largest_key;
}

class VectorInternalIterator : public InternalIterator {
 public:
  VectorInternalIterator(std::vector<VersionedEntry> entries,
                         SequenceNumber read_sequence,
                         Status status)
      : status_(std::move(status)) {
    for (auto& entry : entries) {
      if (entry.sequence <= read_sequence) {
        entries_.push_back(std::move(entry));
      }
    }
  }

  void SeekToFirst() override {
    index_ = 0;
  }

  void Seek(const std::string& target) override {
    index_ = static_cast<size_t>(
        std::lower_bound(entries_.begin(), entries_.end(), target,
                         [](const VersionedEntry& entry, const std::string& key) {
                           return entry.key < key;
                         }) -
        entries_.begin());
  }

  void Next() override {
    if (Valid()) {
      ++index_;
    }
  }

  bool Valid() const override {
    return status_.ok() && index_ < entries_.size();
  }

  const std::string& key() const override {
    static const std::string empty;
    return Valid() ? entries_[index_].key : empty;
  }

  const std::string& value() const override {
    static const std::string empty;
    return Valid() ? entries_[index_].value : empty;
  }

  SequenceNumber sequence() const override {
    return Valid() ? entries_[index_].sequence : 0;
  }

  bool deleted() const override {
    return Valid() && entries_[index_].deleted;
  }

  Status status() const override {
    return status_;
  }

 private:
  std::vector<VersionedEntry> entries_;
  size_t index_ = 0;
  Status status_;
};

class MergingInternalIterator : public InternalIterator {
 public:
  explicit MergingInternalIterator(std::vector<std::unique_ptr<InternalIterator>> children)
      : children_(std::move(children)), heap_(HeapCompare{&children_}) {}

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    RebuildHeap();
    AdvanceToNextVisible();
  }

  void Seek(const std::string& target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    RebuildHeap();
    AdvanceToNextVisible();
  }

  void Next() override {
    if (!Valid()) {
      return;
    }
    AdvanceToNextVisible();
  }

  bool Valid() const override {
    return status_.ok() && valid_;
  }

  const std::string& key() const override {
    static const std::string empty;
    return Valid() ? current_.key : empty;
  }

  const std::string& value() const override {
    static const std::string empty;
    return Valid() ? current_.value : empty;
  }

  SequenceNumber sequence() const override {
    return Valid() ? current_.sequence : 0;
  }

  bool deleted() const override {
    return false;
  }

  Status status() const override {
    return status_;
  }

 private:
  struct HeapCompare {
    const std::vector<std::unique_ptr<InternalIterator>>* children = nullptr;

    bool operator()(size_t lhs, size_t rhs) const {
      const auto& left = (*children)[lhs];
      const auto& right = (*children)[rhs];
      if (left->key() != right->key()) {
        return left->key() > right->key();
      }
      return left->sequence() < right->sequence();
    }
  };

  void RebuildHeap() {
    heap_ = std::priority_queue<size_t, std::vector<size_t>, HeapCompare>(
        HeapCompare{&children_});
    valid_ = false;
    status_ = Status::OK();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (!children_[i]->status().ok()) {
        status_ = children_[i]->status();
        return;
      }
      if (children_[i]->Valid()) {
        heap_.push(i);
      }
    }
  }

  void AdvanceChildPastKey(size_t child_index, const std::string& key) {
    auto& child = children_[child_index];
    while (child->Valid() && child->key() == key) {
      child->Next();
    }
    if (!child->status().ok()) {
      status_ = child->status();
      return;
    }
    if (child->Valid()) {
      heap_.push(child_index);
    }
  }

  void AdvanceToNextVisible() {
    valid_ = false;
    while (status_.ok() && !heap_.empty()) {
      const size_t first_child = heap_.top();
      const std::string next_key = children_[first_child]->key();
      const VersionedEntry selected{next_key,
                                    children_[first_child]->sequence(),
                                    children_[first_child]->value(),
                                    children_[first_child]->deleted()};

      while (!heap_.empty() && children_[heap_.top()]->key() == next_key) {
        const size_t child_index = heap_.top();
        heap_.pop();
        AdvanceChildPastKey(child_index, next_key);
      }

      if (!selected.deleted) {
        current_ = selected;
        valid_ = true;
        return;
      }
    }
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::priority_queue<size_t, std::vector<size_t>, HeapCompare> heap_;
  VersionedEntry current_;
  bool valid_ = false;
  Status status_;
};

}  // namespace

Iterator::Iterator(std::unique_ptr<InternalIterator> impl)
    : impl_(std::move(impl)) {}

Iterator::~Iterator() = default;

void Iterator::SeekToFirst() {
  impl_->SeekToFirst();
}

void Iterator::Seek(const std::string& target) {
  impl_->Seek(target);
}

void Iterator::Next() {
  impl_->Next();
}

bool Iterator::Valid() const {
  return impl_->Valid();
}

const std::string& Iterator::key() const {
  return impl_->key();
}

const std::string& Iterator::value() const {
  return impl_->value();
}

Status Iterator::status() const {
  return impl_->status();
}

std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound: " + message_;
    case Code::kIOError:
      return "IOError: " + message_;
    case Code::kCorruption:
      return "Corruption: " + message_;
    case Code::kInvalidArgument:
      return "InvalidArgument: " + message_;
  }
  return "Unknown";
}

DB::DB(std::filesystem::path db_path)
    : DB(Options{std::move(db_path)}) {}

DB::DB(Options options)
    : options_(std::move(options)),
      active_memtable_(std::make_shared<MemTable>()),
      manifest_(std::make_unique<Manifest>(options_.db_path)),
      block_cache_(std::make_shared<BlockCache>(options_.block_cache_capacity)) {}

DB::~DB() {
  (void)Close();
}

Status DB::Open() {
  std::lock_guard<std::mutex> lock(version_mu_);
  if (!closed_) {
    return Status::InvalidArgument("database is already open");
  }
  if (options_.db_path.empty()) {
    return Status::InvalidArgument("db_path cannot be empty");
  }
  if (options_.error_if_exists && std::filesystem::exists(options_.db_path)) {
    return Status::InvalidArgument("database already exists");
  }
  if (!options_.create_if_missing && !std::filesystem::exists(options_.db_path)) {
    return Status::IOError("database does not exist: " + options_.db_path.string());
  }
  std::filesystem::create_directories(options_.db_path);

  active_memtable_ = std::make_shared<MemTable>();
  immutable_memtables_.clear();
  wal_.reset();
  {
    std::lock_guard<std::mutex> bg_lock(bg_mu_);
    background_stop_ = false;
    background_work_pending_ = false;
  }
  background_status_ = Status::OK();
  active_wal_file_number_ = 1;
  active_memtable_oldest_wal_file_number_ = 1;

  Status status = LoadOrRecoverManifest();
  if (!status.ok()) {
    return status;
  }

  status = OpenSSTables();
  if (!status.ok()) {
    return status;
  }
  status = Recover();
  if (!status.ok()) {
    return status;
  }
  if (manifest_->NextFileNumber() <= active_wal_file_number_) {
    manifest_->SetNextFileNumber(active_wal_file_number_ + 1);
    status = SaveManifest();
    if (!status.ok()) {
      return status;
    }
  }

  wal_ = std::make_unique<WALWriter>(WALPath(active_wal_file_number_));
  status = wal_->Open();
  if (!status.ok()) {
    return status;
  }
  closed_ = false;
  background_worker_ = std::thread(&DB::BackgroundWorkerLoop, this);
  return Status::OK();
}

Status DB::Close() {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::unique_lock<std::mutex> lock(version_mu_);
  if (closed_) {
    return background_status_;
  }

  bool moved_active_memtable = false;
  {
    std::unique_lock<std::shared_mutex> memtable_lock(memtable_mu_);
    if (active_memtable_ != nullptr && active_memtable_->Size() > 0) {
      if (wal_ != nullptr) {
        wal_->Close();
        wal_.reset();
      }
      immutable_memtables_.push_back(
          ImmutableMemTable{std::move(active_memtable_),
                            active_memtable_oldest_wal_file_number_});
      active_memtable_ = std::make_shared<MemTable>();
      moved_active_memtable = true;
    } else if (wal_ != nullptr) {
      wal_->Close();
    }
  }

  {
    std::lock_guard<std::mutex> bg_lock(bg_mu_);
    background_stop_ = true;
    background_work_pending_ = true;
  }
  background_cv_.notify_all();
  lock.unlock();

  if (background_worker_.joinable()) {
    background_worker_.join();
  }

  lock.lock();
  Status status = background_status_;
  if (wal_ != nullptr) {
    wal_->Close();
    wal_.reset();
  }
  if (status.ok() && !immutable_memtables_.empty()) {
    status = Status::IOError("background flush did not drain all immutable memtables");
  }
  if (status.ok() && moved_active_memtable) {
    const std::uint64_t next_wal_file_number = manifest_->AllocateFileNumber();
    manifest_->SetWALFileNumber(next_wal_file_number);
    active_wal_file_number_ = next_wal_file_number;
    active_memtable_oldest_wal_file_number_ = next_wal_file_number;
    status = SaveManifest();
    if (status.ok()) {
      status = RemoveObsoleteWALFiles(next_wal_file_number);
    }
  }
  closed_ = true;
  return status;
}

Status DB::Write(const WriteBatch& batch) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::lock_guard<std::mutex> lock(version_mu_);

  if (batch.Count() == 0) {
    return Status::OK();
  }
  if (!background_status_.ok()) {
    return background_status_;
  }
  if (wal_ == nullptr) {
    return Status::IOError("database is not open");
  }

  for (const auto& operation : batch.Operations()) {
    Status status = ValidateKey(operation.key);
    if (!status.ok()) {
      return status;
    }
    switch (operation.type) {
      case WriteBatchOpType::kPut:
      case WriteBatchOpType::kDelete:
        break;
      default:
        return Status::InvalidArgument("unknown WriteBatch operation type");
    }
  }

  const SequenceNumber first_sequence = manifest_->LastSequence() + 1;
  Status status = wal_->AppendBatch(batch, first_sequence);
  if (!status.ok()) {
    return status;
  }

  {
    std::unique_lock<std::shared_mutex> memtable_lock(memtable_mu_);
    SequenceNumber sequence = first_sequence;
    for (const auto& operation : batch.Operations()) {
      switch (operation.type) {
        case WriteBatchOpType::kPut:
          active_memtable_->Put(operation.key, sequence, operation.value);
          break;
        case WriteBatchOpType::kDelete:
          active_memtable_->Delete(operation.key, sequence);
          break;
      }
      manifest_->SetLastSequence(sequence);
      ++sequence;
    }

    status = MaybeFlushMemTable();
  }
  return status;
}

Status DB::Put(const std::string& key, const std::string& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(batch);
}

Status DB::Get(const std::string& key, std::string* value) {
  return Get(key, value, ReadOptions{});
}

Status DB::Get(const std::string& key, std::string* value, const ReadOptions& options) {
  Status status = ValidateKey(key);
  if (!status.ok()) {
    return status;
  }

  SequenceNumber read_sequence = 0;
  std::shared_ptr<MemTable> active;
  std::vector<std::shared_ptr<MemTable>> immutable_memtables;
  std::vector<std::shared_ptr<SSTable>> sstables;
  {
    std::lock_guard<std::mutex> lock(version_mu_);
    read_sequence = ReadSequence(options);
    active = active_memtable_;
    immutable_memtables.reserve(immutable_memtables_.size());
    for (const auto& immutable : immutable_memtables_) {
      immutable_memtables.push_back(immutable.memtable);
    }
    sstables = sstables_;
  }

  if (options_.testing_after_read_version_pin) {
    options_.testing_after_read_version_pin();
  }

  {
    std::shared_lock<std::shared_mutex> memtable_lock(memtable_mu_);
    auto entry = active->Get(key, read_sequence);
    if (entry.has_value()) {
      if (entry->deleted) {
        return Status::NotFound("key deleted");
      }

      *value = entry->value;
      return Status::OK();
    }

    for (auto it = immutable_memtables.rbegin(); it != immutable_memtables.rend(); ++it) {
      entry = (*it)->Get(key, read_sequence);
      if (entry.has_value()) {
        if (entry->deleted) {
          return Status::NotFound("key deleted");
        }

        *value = entry->value;
        return Status::OK();
      }
    }
  }

  for (auto it = sstables.rbegin(); it != sstables.rend(); ++it) {
    bool found = false;
    status = (*it)->Get(key, read_sequence, value, &found);
    if (status.ok()) {
      return status;
    }
    if (found && status.IsNotFound()) {
      return status;
    }
    if (!status.IsNotFound()) {
      return status;
    }
  }

  return Status::NotFound("key not found");
}

Status DB::Delete(const std::string& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(batch);
}

Status DB::Compact() {
  std::lock_guard<std::mutex> lock(version_mu_);
  return CompactUnlocked();
}

Status DB::CompactUnlocked() {
  if (sstables_.empty()) {
    return Status::OK();
  }

  const VersionEdit old_edit = manifest_->CurrentEdit();
  const auto& metas = old_edit.sstables;
  std::vector<SSTableMeta> inputs;
  int target_level = 1;

  auto add_overlapping = [&](int level, const SSTableMeta& range) {
    for (const auto& meta : metas) {
      if (meta.level == level && RangesOverlap(meta, range)) {
        auto exists = std::find_if(inputs.begin(), inputs.end(),
                                   [&](const SSTableMeta& selected) {
                                     return selected.file_number == meta.file_number;
                                   });
        if (exists == inputs.end()) {
          inputs.push_back(meta);
        }
      }
    }
  };

  for (const auto& meta : metas) {
    if (meta.level == 0) {
      inputs.push_back(meta);
    }
  }
  if (!inputs.empty()) {
    target_level = 1;
    SSTableMeta combined_range = inputs.front();
    for (const auto& meta : inputs) {
      if (combined_range.smallest_key.empty() ||
          (!meta.smallest_key.empty() && meta.smallest_key < combined_range.smallest_key)) {
        combined_range.smallest_key = meta.smallest_key;
      }
      if (combined_range.largest_key.empty() ||
          (!meta.largest_key.empty() && meta.largest_key > combined_range.largest_key)) {
        combined_range.largest_key = meta.largest_key;
      }
    }
    add_overlapping(1, combined_range);
  } else {
    for (const auto& meta : metas) {
      if (meta.level == 1) {
        inputs.push_back(meta);
      }
    }
    target_level = 2;
    if (!inputs.empty()) {
      SSTableMeta combined_range = inputs.front();
      for (const auto& meta : inputs) {
        if (combined_range.smallest_key.empty() ||
            (!meta.smallest_key.empty() && meta.smallest_key < combined_range.smallest_key)) {
          combined_range.smallest_key = meta.smallest_key;
        }
        if (combined_range.largest_key.empty() ||
            (!meta.largest_key.empty() && meta.largest_key > combined_range.largest_key)) {
          combined_range.largest_key = meta.largest_key;
        }
      }
      add_overlapping(2, combined_range);
    }
  }

  if (inputs.empty()) {
    return Status::OK();
  }

  std::set<std::uint64_t> input_numbers;
  for (const auto& meta : inputs) {
    input_numbers.insert(meta.file_number);
  }
  LogEvent("compaction start inputs " + std::to_string(input_numbers.size()) +
           " -> L" + std::to_string(target_level));

  std::vector<std::unique_ptr<InternalIterator>> compaction_inputs;
  for (const auto& table : sstables_) {
    if (input_numbers.find(table->FileNumber()) == input_numbers.end()) {
      continue;
    }
    auto iter = table->NewIterator(std::numeric_limits<SequenceNumber>::max());
    iter->SeekToFirst();
    if (!iter->status().ok()) {
      return iter->status();
    }
    if (iter->Valid()) {
      compaction_inputs.push_back(std::move(iter));
    }
  }

  std::vector<VersionedEntry> output;
  const SequenceNumber min_snapshot = MinActiveSnapshotSequence();
  struct CompactionHeapCompare {
    const std::vector<std::unique_ptr<InternalIterator>>* inputs = nullptr;

    bool operator()(size_t lhs, size_t rhs) const {
      const auto& left = (*inputs)[lhs];
      const auto& right = (*inputs)[rhs];
      if (left->key() != right->key()) {
        return left->key() > right->key();
      }
      return left->sequence() < right->sequence();
    }
  };
  std::priority_queue<size_t, std::vector<size_t>, CompactionHeapCompare> heap(
      CompactionHeapCompare{&compaction_inputs});
  for (size_t i = 0; i < compaction_inputs.size(); ++i) {
    heap.push(i);
  }

  while (!heap.empty()) {
    const std::string current_key = compaction_inputs[heap.top()]->key();
    std::vector<VersionedEntry> versions;
    while (!heap.empty() && compaction_inputs[heap.top()]->key() == current_key) {
      const size_t child_index = heap.top();
      heap.pop();
      auto& iter = compaction_inputs[child_index];
      while (iter->Valid() && iter->key() == current_key) {
        versions.push_back(VersionedEntry{iter->key(),
                                          iter->sequence(),
                                          iter->value(),
                                          iter->deleted()});
        iter->Next();
      }
      if (!iter->status().ok()) {
        return iter->status();
      }
      if (iter->Valid()) {
        heap.push(child_index);
      }
    }
    std::sort(versions.begin(), versions.end(),
              [](const VersionedEntry& lhs, const VersionedEntry& rhs) {
                if (lhs.sequence != rhs.sequence) {
                  return lhs.sequence > rhs.sequence;
                }
                return lhs.key < rhs.key;
              });

    if (min_snapshot == 0) {
      if (!versions.front().deleted || target_level < 2) {
        output.push_back(std::move(versions.front()));
      }
      continue;
    }

    bool kept_boundary = false;
    for (auto& version : versions) {
      if (version.sequence > min_snapshot) {
        output.push_back(std::move(version));
      } else if (!kept_boundary) {
        output.push_back(std::move(version));
        kept_boundary = true;
      }
    }
  }
  std::sort(output.begin(), output.end(),
            [](const VersionedEntry& lhs, const VersionedEntry& rhs) {
              if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
              }
              return lhs.sequence > rhs.sequence;
            });

  const std::vector<std::shared_ptr<SSTable>> old_tables = sstables_;
  const std::uint64_t new_file_number = manifest_->AllocateFileNumber();
  const std::filesystem::path new_path = SSTablePath(new_file_number);
  Status status = SSTable::CreateFromEntries(new_path, output,
                                             options_.block_size,
                                             options_.bloom_bits_per_key);
  if (!status.ok()) {
    return status;
  }

  std::shared_ptr<SSTable> new_table;
  status = SSTable::Open(new_file_number, new_path, block_cache_, &new_table);
  if (!status.ok()) {
    return status;
  }

  sstables_.erase(std::remove_if(sstables_.begin(), sstables_.end(),
                                 [&](const std::shared_ptr<SSTable>& table) {
                                   return input_numbers.find(table->FileNumber()) !=
                                          input_numbers.end();
                                 }),
                  sstables_.end());
  sstables_.push_back(new_table);
  std::vector<SSTableMeta> new_metas;
  for (const auto& meta : old_edit.sstables) {
    if (input_numbers.find(meta.file_number) == input_numbers.end()) {
      new_metas.push_back(meta);
    }
  }
  SSTableMeta new_meta;
  new_meta.file_number = new_table->FileNumber();
  new_meta.file_path = new_table->FilePath();
  new_meta.level = target_level;
  std::error_code file_size_ec;
  new_meta.file_size = std::filesystem::file_size(new_meta.file_path, file_size_ec);
  if (file_size_ec) {
    new_meta.file_size = 0;
  }
  if (!output.empty()) {
    auto minmax = std::minmax_element(
        output.begin(), output.end(),
        [](const VersionedEntry& lhs, const VersionedEntry& rhs) {
          return lhs.key < rhs.key;
        });
    new_meta.smallest_key = minmax.first->key;
    new_meta.largest_key = minmax.second->key;
  }
  new_metas.push_back(std::move(new_meta));
  manifest_->SetSSTables(std::move(new_metas));
  manifest_->SetLastSequence(old_edit.last_sequence);
  status = SaveManifest();
  if (!status.ok()) {
    sstables_ = old_tables;
    manifest_->SetCurrentEdit(old_edit);
    return status;
  }

  for (const auto& table : old_tables) {
    if (input_numbers.find(table->FileNumber()) != input_numbers.end()) {
      Status remove_status = RemoveFileIfExists(table->FilePath());
      if (!remove_status.ok()) {
        return remove_status;
      }
    }
  }
  ++compaction_count_;
  LogEvent("compaction end output SSTable " + std::to_string(new_file_number) +
           " L" + std::to_string(target_level));
  return Status::OK();
}

DBStats DB::Stats() const {
  std::lock_guard<std::mutex> lock(version_mu_);
  DBStats stats;
  stats.sstable_count = sstables_.size();
  stats.compaction_count = compaction_count_;
  stats.sstable_full_scans = sstable_full_scans_;
  for (const auto& meta : manifest_->SSTables()) {
    if (meta.level == 0) {
      ++stats.level0_sstable_count;
    } else if (meta.level == 1) {
      ++stats.level1_sstable_count;
    } else if (meta.level == 2) {
      ++stats.level2_sstable_count;
    }
  }
  if (block_cache_ != nullptr) {
    const BlockCacheStats cache_stats = block_cache_->Stats();
    stats.cache_hits = cache_stats.hits;
    stats.cache_misses = cache_stats.misses;
  }
  for (const auto& table : sstables_) {
    stats.bloom_filtered += table->BloomFilteredCount();
    stats.block_reads += table->BlockReadCount();
    stats.block_restart_seeks += table->RestartSeekCount();
  }
  return stats;
}

Status DB::LoadOrRecoverManifest() {
  if (manifest_->Exists()) {
    return manifest_->Load();
  }

  VersionEdit edit;
  std::vector<SSTableMeta> tables;
  std::uint64_t max_sstable_number = 0;
  std::uint64_t min_wal_number = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_wal_number = 0;

  for (const auto& entry : std::filesystem::directory_iterator(options_.db_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    std::uint64_t number = 0;
    if (ParseSSTableFileName(entry.path(), &number)) {
      SSTableMeta meta;
      meta.file_number = number;
      meta.file_path = entry.path();
      meta.level = 0;
      std::error_code ec;
      meta.file_size = std::filesystem::file_size(entry.path(), ec);
      if (ec) {
        meta.file_size = 0;
      }
      tables.push_back(std::move(meta));
      max_sstable_number = std::max(max_sstable_number, number);
    } else if (ParseWALFileName(entry.path(), &number)) {
      min_wal_number = std::min(min_wal_number, number);
      max_wal_number = std::max(max_wal_number, number);
    } else if (entry.path().filename() == "wal.log") {
      min_wal_number = std::min<std::uint64_t>(min_wal_number, 1);
      max_wal_number = std::max<std::uint64_t>(max_wal_number, 1);
    }
  }

  std::sort(tables.begin(), tables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  SequenceNumber max_sequence = 0;
  for (auto& meta : tables) {
    std::shared_ptr<SSTable> table;
    Status status = SSTable::Open(meta.file_number, meta.file_path, block_cache_, &table);
    if (!status.ok()) {
      return status;
    }
    std::vector<VersionedEntry> entries;
    ++sstable_full_scans_;
    status = table->Entries(&entries);
    if (!status.ok()) {
      return status;
    }
    for (const auto& entry : entries) {
      max_sequence = std::max(max_sequence, entry.sequence);
    }
    if (!entries.empty()) {
      auto minmax = std::minmax_element(
          entries.begin(), entries.end(),
          [](const VersionedEntry& lhs, const VersionedEntry& rhs) {
            return lhs.key < rhs.key;
          });
      meta.smallest_key = minmax.first->key;
      meta.largest_key = minmax.second->key;
    }
  }
  if (max_wal_number == 0) {
    min_wal_number = 1;
    max_wal_number = 1;
  }
  edit.sstables = std::move(tables);
  edit.wal_file_number = min_wal_number;
  edit.last_sequence = max_sequence;
  edit.next_file_number = max_sstable_number + 1;
  edit.next_file_number = std::max(edit.next_file_number, max_wal_number + 1);
  return manifest_->Save(edit);
}

Status DB::OpenSSTables() {
  sstables_.clear();
  for (const auto& meta : manifest_->SSTables()) {
    std::shared_ptr<SSTable> table;
    Status status = SSTable::Open(meta.file_number, meta.file_path, block_cache_, &table);
    if (!status.ok()) {
      return status;
    }
    sstables_.push_back(std::move(table));
  }
  return Status::OK();
}

Status DB::Recover() {
  std::vector<std::pair<std::uint64_t, std::filesystem::path>> wal_files;
  const std::uint64_t oldest_live_wal = manifest_->WALFileNumber();
  std::uint64_t max_seen_file_number = 0;
  for (const auto& meta : manifest_->SSTables()) {
    max_seen_file_number = std::max(max_seen_file_number, meta.file_number);
  }

  for (const auto& entry : std::filesystem::directory_iterator(options_.db_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    std::uint64_t number = 0;
    if (ParseWALFileName(entry.path(), &number)) {
      max_seen_file_number = std::max(max_seen_file_number, number);
      if (number >= oldest_live_wal) {
        wal_files.push_back({number, entry.path()});
      }
    } else if (entry.path().filename() == "wal.log" && oldest_live_wal <= 1) {
      wal_files.push_back({1, entry.path()});
      max_seen_file_number = std::max<std::uint64_t>(max_seen_file_number, 1);
    }
  }

  std::sort(wal_files.begin(), wal_files.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first < rhs.first;
            });

  active_wal_file_number_ = wal_files.empty() ? oldest_live_wal : wal_files.back().first;
  active_memtable_oldest_wal_file_number_ =
      wal_files.empty() ? oldest_live_wal : wal_files.front().first;
  if (manifest_->NextFileNumber() <= max_seen_file_number) {
    manifest_->SetNextFileNumber(max_seen_file_number + 1);
  }

  for (const auto& wal_file : wal_files) {
    LogEvent("recovery replay WAL " + wal_file.second.filename().string());
    WALReader reader(wal_file.second);
    while (true) {
      WALBatchRecord record;
      bool eof = false;
      Status status = reader.ReadNext(&record, &eof);
      if (!status.ok()) {
        return status;
      }
      if (eof) {
        break;
      }

      SequenceNumber sequence = record.sequence;
      for (const auto& operation : record.batch.Operations()) {
        switch (operation.type) {
          case WriteBatchOpType::kPut:
            active_memtable_->Put(operation.key, sequence, operation.value);
            break;
          case WriteBatchOpType::kDelete:
            active_memtable_->Delete(operation.key, sequence);
            break;
        }
        if (sequence > manifest_->LastSequence()) {
          manifest_->SetLastSequence(sequence);
        }
        ++sequence;
      }
    }
  }

  return Status::OK();
}

Status DB::MaybeFlushMemTable() {
  if (active_memtable_->Size() < options_.memtable_entries_limit) {
    return Status::OK();
  }
  return FlushMemTable();
}

Status DB::FlushMemTable() {
  if (active_memtable_->Size() == 0) {
    return Status::OK();
  }

  const VersionEdit old_edit = manifest_->CurrentEdit();
  const std::uint64_t old_wal_number = active_memtable_oldest_wal_file_number_;
  const std::uint64_t new_wal_number = manifest_->AllocateFileNumber();
  LogEvent("memtable switch active WAL " + std::to_string(old_wal_number) +
           " -> " + std::to_string(new_wal_number));
  auto new_wal = std::make_unique<WALWriter>(WALPath(new_wal_number));
  Status status = new_wal->Open();
  if (!status.ok()) {
    manifest_->SetCurrentEdit(old_edit);
    return status;
  }

  status = SaveManifest();
  if (!status.ok()) {
    manifest_->SetCurrentEdit(old_edit);
    return status;
  }

  if (wal_ != nullptr) {
    wal_->Close();
  }
  immutable_memtables_.push_back(ImmutableMemTable{active_memtable_, old_wal_number});
  active_memtable_ = std::make_shared<MemTable>();
  active_wal_file_number_ = new_wal_number;
  active_memtable_oldest_wal_file_number_ = new_wal_number;
  wal_ = std::move(new_wal);
  {
    std::lock_guard<std::mutex> bg_lock(bg_mu_);
    background_work_pending_ = true;
  }
  background_cv_.notify_one();
  return Status::OK();
}

void DB::BackgroundWorkerLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> bg_lock(bg_mu_);
      background_cv_.wait(bg_lock, [&] {
        return background_stop_ || background_work_pending_;
      });
      background_work_pending_ = false;
    }

    while (true) {
      ImmutableMemTable immutable;
      std::uint64_t sstable_number = 0;
      {
        std::unique_lock<std::mutex> lock(version_mu_);
        if (immutable_memtables_.empty()) {
          std::lock_guard<std::mutex> bg_lock(bg_mu_);
          if (background_stop_) {
            return;
          }
          break;
        }
        if (!background_status_.ok()) {
          return;
        }

        immutable = immutable_memtables_.front();
        sstable_number = manifest_->AllocateFileNumber();
        LogEvent("flush start WAL " + std::to_string(immutable.wal_file_number) +
                 " -> SSTable " + std::to_string(sstable_number));
      }

      std::shared_ptr<SSTable> table;
      SSTableMeta table_meta;
      Status status =
          FlushImmutableMemTable(immutable, sstable_number, &table, &table_meta);

      std::uint64_t oldest_live_wal_after_flush = 0;
      bool cleanup_obsolete_wals = false;
      {
        std::unique_lock<std::mutex> lock(version_mu_);
        if (!status.ok()) {
          SetBackgroundErrorLocked(status);
          continue;
        }
        if (immutable_memtables_.empty() ||
            immutable_memtables_.front().wal_file_number != immutable.wal_file_number) {
          SetBackgroundErrorLocked(
              Status::Corruption("immutable memtable queue changed during flush"));
          continue;
        }

        const VersionEdit old_edit = manifest_->CurrentEdit();
        const auto old_tables = sstables_;
        sstables_.push_back(table);
        std::vector<SSTableMeta> new_metas = old_edit.sstables;
        new_metas.push_back(table_meta);
        manifest_->SetSSTables(std::move(new_metas));
        const std::uint64_t next_live_wal =
            immutable_memtables_.size() > 1
                ? immutable_memtables_[1].wal_file_number
                : active_memtable_oldest_wal_file_number_;
        manifest_->SetWALFileNumber(next_live_wal);

        status = SaveManifest();
        if (!status.ok()) {
          sstables_ = old_tables;
          manifest_->SetCurrentEdit(old_edit);
          SetBackgroundErrorLocked(status);
          continue;
        }

        immutable_memtables_.pop_front();
        oldest_live_wal_after_flush = next_live_wal;
        cleanup_obsolete_wals = true;

        status = MaybeCompact();
        if (!status.ok()) {
          SetBackgroundErrorLocked(status);
        }
      }

      if (cleanup_obsolete_wals) {
        status = RemoveObsoleteWALFiles(oldest_live_wal_after_flush);
        if (!status.ok()) {
          std::lock_guard<std::mutex> lock(version_mu_);
          SetBackgroundErrorLocked(status);
        }
      }
      LogEvent("flush end WAL " + std::to_string(immutable.wal_file_number) +
               " -> SSTable " + std::to_string(sstable_number));
    }
  }
}

Status DB::FlushImmutableMemTable(const ImmutableMemTable& immutable,
                                  std::uint64_t sstable_number,
                                  std::shared_ptr<SSTable>* table,
                                  SSTableMeta* meta) {
  if (options_.testing_fail_flush) {
    return Status::IOError("injected flush failure");
  }
  const std::vector<VersionedEntry> entries = immutable.memtable->Entries();
  const std::filesystem::path sstable_path = SSTablePath(sstable_number);
  Status status = SSTable::CreateFromEntries(sstable_path,
                                             entries,
                                             options_.block_size,
                                             options_.bloom_bits_per_key);
  if (!status.ok()) {
    return status;
  }

  status = SSTable::Open(sstable_number, sstable_path, block_cache_, table);
  if (!status.ok()) {
    return status;
  }

  meta->file_number = (*table)->FileNumber();
  meta->file_path = (*table)->FilePath();
  meta->level = 0;
  std::error_code ec;
  meta->file_size = std::filesystem::file_size(meta->file_path, ec);
  if (ec) {
    meta->file_size = 0;
  }
  if (!entries.empty()) {
    meta->smallest_key = entries.front().key;
    meta->largest_key = entries.back().key;
  }
  return Status::OK();
}

void DB::SetBackgroundErrorLocked(Status status) {
  if (background_status_.ok()) {
    background_status_ = std::move(status);
  }
}

Status DB::MaybeCompact() {
  for (int attempts = 0; attempts < 4; ++attempts) {
    size_t level0_count = 0;
    std::uint64_t level1_size = 0;
    for (const auto& meta : manifest_->SSTables()) {
      if (meta.level == 0) {
        ++level0_count;
      } else if (meta.level == 1) {
        level1_size += meta.file_size;
      }
    }

    const std::uint64_t level1_size_limit =
        static_cast<std::uint64_t>(options_.memtable_entries_limit) * 64;
    if (level0_count > options_.level0_sstable_limit ||
        level1_size > level1_size_limit) {
      Status status = CompactUnlocked();
      if (!status.ok()) {
        return status;
      }
      continue;
    }
    return Status::OK();
  }
  return Status::OK();
}

SequenceNumber DB::ReadSequence(const ReadOptions& options) const {
  return options.snapshot == nullptr ? manifest_->LastSequence()
                                     : options.snapshot->sequence();
}

SequenceNumber DB::MinActiveSnapshotSequence() const {
  SequenceNumber min_sequence = 0;
  for (const auto& snapshot : snapshots_) {
    if (snapshot == nullptr) {
      continue;
    }
    if (min_sequence == 0 || snapshot->sequence() < min_sequence) {
      min_sequence = snapshot->sequence();
    }
  }
  return min_sequence;
}

const Snapshot* DB::GetSnapshot() {
  std::lock_guard<std::mutex> lock(version_mu_);
  snapshots_.push_back(std::unique_ptr<Snapshot>(new Snapshot(manifest_->LastSequence())));
  return snapshots_.back().get();
}

void DB::ReleaseSnapshot(const Snapshot* snapshot) {
  std::lock_guard<std::mutex> lock(version_mu_);
  auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                         [snapshot](const std::unique_ptr<Snapshot>& candidate) {
                           return candidate.get() == snapshot;
                         });
  if (it != snapshots_.end()) {
    snapshots_.erase(it);
  }
}

std::unique_ptr<Iterator> DB::NewIterator(const ReadOptions& options) {
  std::lock_guard<std::mutex> lock(version_mu_);
  const SequenceNumber read_sequence = ReadSequence(options);
  std::vector<std::unique_ptr<InternalIterator>> children;

  for (const auto& table : sstables_) {
    children.push_back(table->NewIterator(read_sequence));
  }
  for (const auto& immutable : immutable_memtables_) {
    children.push_back(std::make_unique<VectorInternalIterator>(
        immutable.memtable->Entries(), read_sequence, Status::OK()));
  }
  children.push_back(std::make_unique<VectorInternalIterator>(
      active_memtable_->Entries(), read_sequence, Status::OK()));

  return std::make_unique<Iterator>(
      std::make_unique<MergingInternalIterator>(std::move(children)));
}

Status DB::SaveManifest() {
  LogEvent("manifest edit append");
  return manifest_->Save(manifest_->CurrentEdit());
}

Status DB::RemoveObsoleteWALFiles(std::uint64_t oldest_live_wal) {
  bool removed_any = false;
  for (const auto& entry : std::filesystem::directory_iterator(options_.db_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }

    std::uint64_t number = 0;
    const bool numbered_wal = ParseWALFileName(entry.path(), &number);
    const bool legacy_wal = entry.path().filename() == "wal.log";
    if ((!numbered_wal || number >= oldest_live_wal) &&
        (!legacy_wal || oldest_live_wal <= 1)) {
      continue;
    }

    Status status = RemoveFileIfExists(entry.path());
    if (!status.ok()) {
      return status;
    }
    removed_any = true;
  }
  return removed_any ? FsyncDirectory(options_.db_path) : Status::OK();
}

void DB::LogEvent(const std::string& message) const {
  if (options_.enable_event_log) {
    std::clog << "[kvdb] " << message << '\n';
  }
}

std::filesystem::path DB::SSTablePath(std::uint64_t number) const {
  return options_.db_path / SSTableFileName(number);
}

std::filesystem::path DB::WALPath(std::uint64_t number) const {
  return options_.db_path / WALFileName(number);
}

}  // namespace kv
