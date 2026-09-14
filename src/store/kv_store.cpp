#include "store/kv_store.h"

#include "persistence/snapshot.h"
#include "persistence/wal.h"

#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace kv::store {

std::size_t KVStore::ShardIndex(const std::string& key) {
  return std::hash<std::string>{}(key) & (kShardCount - 1);
}

std::vector<std::unique_lock<std::shared_mutex>> KVStore::LockAllShardsExclusive() {
  std::vector<std::unique_lock<std::shared_mutex>> locks;
  locks.reserve(kShardCount);
  for (auto& shard : shards_) locks.emplace_back(shard.mutex);
  return locks;
}

std::vector<std::shared_lock<std::shared_mutex>> KVStore::LockAllShardsShared() const {
  std::vector<std::shared_lock<std::shared_mutex>> locks;
  locks.reserve(kShardCount);
  for (const auto& shard : shards_) locks.emplace_back(shard.mutex);
  return locks;
}

std::unordered_map<std::string, std::string> KVStore::CollectDataLocked() const {
  std::unordered_map<std::string, std::string> result;
  for (const auto& shard : shards_) {
    result.insert(shard.data.begin(), shard.data.end());
  }
  return result;
}

void KVStore::ReplaceDataLocked(std::unordered_map<std::string, std::string> data) {
  for (auto& shard : shards_) shard.data.clear();
  for (auto& entry : data) {
    shards_[ShardIndex(entry.first)].data.insert(std::move(entry));
  }
}

KVStore::KVStore(persistence::WriteAheadLog* wal, persistence::Snapshot* snapshot)
    : wal_(wal), snapshot_(snapshot) {
  if (snapshot_ != nullptr) {
    checkpoint_thread_ = std::thread(&KVStore::CheckpointLoop, this);
  }
}

KVStore::~KVStore() {
  if (checkpoint_thread_.joinable()) {
    {
      std::lock_guard lock(checkpoint_mutex_);
      checkpoint_stopping_ = true;
    }
    checkpoint_ready_.notify_one();
    checkpoint_thread_.join();
  }
}

KVStore::KVStore(const KVStore& other) {
  std::shared_lock lock(other.mutex_);
  auto shard_locks = other.LockAllShardsShared();
  if (other.snapshot_ != nullptr) {
    throw std::logic_error("cannot copy a store with background checkpointing");
  }
  ReplaceDataLocked(other.CollectDataLocked());
  wal_ = other.wal_;
  snapshot_ = other.snapshot_;
  writes_since_snapshot_ = other.writes_since_snapshot_.load();
}

KVStore& KVStore::operator=(const KVStore& other) {
  if (this == &other) return *this;
  std::unique_lock self_lock(mutex_, std::defer_lock);
  std::shared_lock other_lock(other.mutex_, std::defer_lock);
  std::lock(self_lock, other_lock);
  auto self_shard_locks = LockAllShardsExclusive();
  auto other_shard_locks = other.LockAllShardsShared();
  if (snapshot_ != nullptr || other.snapshot_ != nullptr) {
    throw std::logic_error("cannot copy a store with background checkpointing");
  }
  ReplaceDataLocked(other.CollectDataLocked());
  wal_ = other.wal_;
  snapshot_ = other.snapshot_;
  writes_since_snapshot_ = other.writes_since_snapshot_.load();
  return *this;
}

KVStore::KVStore(KVStore&& other) {
  std::unique_lock lock(other.mutex_);
  auto shard_locks = other.LockAllShardsExclusive();
  if (other.snapshot_ != nullptr) {
    throw std::logic_error("cannot move a store with background checkpointing");
  }
  for (std::size_t i = 0; i < kShardCount; ++i) {
    shards_[i].data = std::move(other.shards_[i].data);
  }
  wal_ = std::exchange(other.wal_, nullptr);
  snapshot_ = std::exchange(other.snapshot_, nullptr);
  writes_since_snapshot_ = other.writes_since_snapshot_.exchange(0);
}

KVStore& KVStore::operator=(KVStore&& other) {
  if (this == &other) return *this;
  std::unique_lock self_lock(mutex_, std::defer_lock);
  std::unique_lock other_lock(other.mutex_, std::defer_lock);
  std::lock(self_lock, other_lock);
  auto self_shard_locks = LockAllShardsExclusive();
  auto other_shard_locks = other.LockAllShardsExclusive();
  if (snapshot_ != nullptr || other.snapshot_ != nullptr) {
    throw std::logic_error("cannot move a store with background checkpointing");
  }
  for (std::size_t i = 0; i < kShardCount; ++i) {
    shards_[i].data = std::move(other.shards_[i].data);
  }
  wal_ = std::exchange(other.wal_, nullptr);
  snapshot_ = std::exchange(other.snapshot_, nullptr);
  writes_since_snapshot_ = other.writes_since_snapshot_.exchange(0);
  return *this;
}

void KVStore::CheckCheckpointError() {
  if (snapshot_ == nullptr) return;
  std::lock_guard lock(checkpoint_mutex_);
  if (checkpoint_error_) std::rethrow_exception(checkpoint_error_);
}

void KVStore::ScheduleCheckpoint() {
  std::lock_guard lock(checkpoint_mutex_);
  checkpoint_requested_ = true;
  checkpoint_ready_.notify_one();
}

void KVStore::CancelPendingCheckpoint() {
  std::lock_guard lock(checkpoint_mutex_);
  ++checkpoint_epoch_;
  checkpoint_requested_ = false;
}

void KVStore::CheckpointLoop() {
  while (true) {
    std::uint64_t epoch;
    {
      std::unique_lock lock(checkpoint_mutex_);
      checkpoint_ready_.wait(lock, [&] {
        return checkpoint_stopping_ || checkpoint_requested_;
      });
      if (checkpoint_stopping_ && !checkpoint_requested_) return;
      checkpoint_requested_ = false;
      checkpoint_active_ = true;
      epoch = checkpoint_epoch_;
    }

    try {
      std::unique_lock snapshot_lock(snapshot_mutex_);
      bool cancelled;
      {
        std::lock_guard lock(checkpoint_mutex_);
        cancelled = epoch != checkpoint_epoch_;
      }
      if (!cancelled) {
        std::unordered_map<std::string, std::string> data;
        std::uint64_t wal_offset = 0;
        {
          std::unique_lock admin_lock(mutex_);
          auto shard_locks = LockAllShardsExclusive();
          data = CollectDataLocked();
          wal_offset = wal_ != nullptr ? wal_->CurrentOffset() : 0;
          writes_since_snapshot_ = 0;
        }
        snapshot_->SaveVerified(data, wal_offset);
      }
    } catch (...) {
      std::lock_guard lock(checkpoint_mutex_);
      checkpoint_error_ = std::current_exception();
      checkpoint_requested_ = false;
    }
    {
      std::lock_guard lock(checkpoint_mutex_);
      checkpoint_active_ = false;
    }
    checkpoint_ready_.notify_all();
  }
}

void KVStore::WaitForCheckpoints() {
  if (snapshot_ == nullptr) return;
  std::unique_lock lock(checkpoint_mutex_);
  checkpoint_ready_.wait(lock, [&] {
    return !checkpoint_requested_ && !checkpoint_active_;
  });
  if (checkpoint_error_) std::rethrow_exception(checkpoint_error_);
}

void KVStore::Set(const std::string& key, const std::string& value) {
  CheckCheckpointError();
  bool checkpoint_due = false;
  {
    Shard& shard = shards_[ShardIndex(key)];
    std::unique_lock shard_lock(shard.mutex);
    if (wal_ != nullptr) wal_->AppendSet(key, value);
    shard.data[key] = value;
    if (snapshot_ != nullptr) {
      checkpoint_due = ++writes_since_snapshot_ == kSnapshotInterval;
    }
    if (checkpoint_due) ScheduleCheckpoint();
  }
}

std::optional<std::string> KVStore::Get(const std::string& key) const {
  const Shard& shard = shards_[ShardIndex(key)];
  std::shared_lock shard_lock(shard.mutex);
  const auto it = shard.data.find(key);
  if (it == shard.data.end()) return std::nullopt;
  return it->second;
}

bool KVStore::Delete(const std::string& key) {
  CheckCheckpointError();
  bool checkpoint_due = false;
  bool erased = false;
  {
    Shard& shard = shards_[ShardIndex(key)];
    std::unique_lock shard_lock(shard.mutex);
    if (wal_ != nullptr) wal_->AppendDelete(key);
    erased = shard.data.erase(key) > 0;
    if (snapshot_ != nullptr) {
      checkpoint_due = ++writes_since_snapshot_ == kSnapshotInterval;
    }
    if (checkpoint_due) ScheduleCheckpoint();
  }
  return erased;
}

bool KVStore::Contains(const std::string& key) const {
  const Shard& shard = shards_[ShardIndex(key)];
  std::shared_lock shard_lock(shard.mutex);
  return shard.data.find(key) != shard.data.end();
}

std::size_t KVStore::Size() const {
  auto shard_locks = LockAllShardsShared();
  std::size_t size = 0;
  for (const auto& shard : shards_) {
    size += shard.data.size();
  }
  return size;
}

void KVStore::Clear() {
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  for (auto& shard : shards_) shard.data.clear();
  writes_since_snapshot_ = 0;
}

void KVStore::ClearPersistence() {
  std::unique_lock snapshot_lock(snapshot_mutex_);
  CancelPendingCheckpoint();
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  if (wal_ != nullptr) wal_->Clear();
  if (snapshot_ != nullptr) snapshot_->Clear();
  writes_since_snapshot_ = 0;
}

bool KVStore::SaveSnapshot() {
  std::unique_lock snapshot_lock(snapshot_mutex_);
  CancelPendingCheckpoint();
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  return SaveSnapshotLocked();
}

bool KVStore::SaveSnapshotLocked() {
  if (snapshot_ == nullptr) return false;
  const std::uint64_t wal_offset = wal_ != nullptr ? wal_->CurrentOffset() : 0;
  snapshot_->SaveVerified(CollectDataLocked(), wal_offset);
  writes_since_snapshot_ = 0;
  return true;
}

bool KVStore::CompactPersistence() {
  std::unique_lock snapshot_lock(snapshot_mutex_);
  CancelPendingCheckpoint();
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  return CompactPersistenceLocked();
}

bool KVStore::CompactPersistenceLocked() {
  if (snapshot_ == nullptr) return false;
  snapshot_->SaveVerified(CollectDataLocked(), 0);
  if (wal_ != nullptr) wal_->Rotate();
  writes_since_snapshot_ = 0;
  return true;
}

kv::persistence::SnapshotLoadResult KVStore::LoadSnapshot(
    const persistence::Snapshot& snapshot) {
  std::unique_lock snapshot_lock(snapshot_mutex_);
  CancelPendingCheckpoint();
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  std::unordered_map<std::string, std::string> data;
  auto result = snapshot.Load(data);
  ReplaceDataLocked(std::move(data));
  return result;
}

std::size_t KVStore::ReplayFromWal(const persistence::WriteAheadLog& wal,
                                   std::uint64_t offset) {
  std::unique_lock admin_lock(mutex_);
  auto shard_locks = LockAllShardsExclusive();
  auto data = CollectDataLocked();
  const std::size_t count = wal.ReplayFrom(offset, data);
  ReplaceDataLocked(std::move(data));
  return count;
}

}  // namespace kv::store
