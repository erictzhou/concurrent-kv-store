#ifndef KV_STORE_STORE_KV_STORE_H_
#define KV_STORE_STORE_KV_STORE_H_

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kv {
namespace persistence {
class Snapshot;
struct SnapshotLoadResult;
class WriteAheadLog;
}  // namespace persistence
}  // namespace kv

namespace kv {
namespace store {

/**
 * @brief Sharded in-memory key-value store with optional WAL and checkpoints.
 *
 * Each key maps deterministically to one of 64 shards. Foreground reads and
 * writes hold only that shard's lock. Global operations acquire all shard
 * locks in ascending order, so they see a consistent in-memory state.
 *
 * WAL submission precedes memory mutation. The shard lock remains held until
 * the WAL policy's acknowledgement boundary, preserving same-key ordering.
 * Snapshot I/O runs on a background thread after a brief global capture.
 * Copy/move is supported only for stores without a configured Snapshot.
 */
class KVStore {
 public:
  /**
   * @brief Constructs an empty key-value store.
   */
  KVStore() = default;

  /**
   * @brief Constructs an empty key-value store with WAL persistence enabled.
   *
   * @param wal Write-ahead log used for future mutations.
   */
  explicit KVStore(persistence::WriteAheadLog* wal,
                   persistence::Snapshot* snapshot = nullptr);
  ~KVStore();

  KVStore(const KVStore& other);
  KVStore& operator=(const KVStore& other);
  KVStore(KVStore&& other);
  KVStore& operator=(KVStore&& other);

  /**
   * @brief Inserts or updates a value for a key.
   *
   * @param key Key to insert or update.
   * @param value Value to associate with the key.
   */
  void Set(const std::string& key, const std::string& value);

  /**
   * @brief Retrieves the value stored for a key.
   *
   * @param key Key to look up.
   * @return Stored value when the key exists, otherwise `std::nullopt`.
   */
  std::optional<std::string> Get(const std::string& key) const;

  /**
   * @brief Removes a key from the store.
   *
   * @param key Key to remove.
   * @return `true` when an entry was removed, otherwise `false`.
   */
  bool Delete(const std::string& key);

  /**
   * @brief Checks whether a key exists in the store.
   *
   * @param key Key to search for.
   * @return `true` when the key exists, otherwise `false`.
   */
  bool Contains(const std::string& key) const;

  /**
   * @brief Returns the number of stored key-value pairs.
   *
   * @return Current entry count.
   */
  std::size_t Size() const;

  /**
   * @brief Removes all entries from the store.
   */
  void Clear();

  /**
   * @brief Clears durable WAL and snapshot files without clearing memory.
   *
   * Future writes continue to use the same WAL and snapshot objects. In-memory
   * data remains available until the process exits or the caller clears it.
   */
  void ClearPersistence();

  /**
   * @brief Writes the current in-memory state to the configured snapshot file.
   *
   * This is a public persistence operation so callers can checkpoint the store
   * without depending on the automatic snapshot interval.
   *
   * @return `true` when a snapshot was configured and written, otherwise false.
   */
  bool SaveSnapshot();

  /**
   * @brief Saves a verified snapshot and rotates WAL history it covers.
   *
   * The snapshot is written with WAL offset zero because rotation starts a new
   * empty WAL. If snapshot writing or verification fails, the WAL is left
   * untouched and the exception is propagated to the caller.
   *
   * @return `true` when a snapshot was configured and compaction completed.
   */
  bool CompactPersistence();

  /** Waits for scheduled automatic checkpoint work and surfaces its failure. */
  void WaitForCheckpoints();

  /**
   * @brief Loads a persisted snapshot directly into this store.
   *
   * Snapshot recovery is separated from WAL replay so startup can restore the
   * latest checkpoint first, then apply WAL operations on top.
   *
   * @param snapshot Snapshot file to load.
   * @return Snapshot metadata, including entry count and covered WAL offset.
   */
  persistence::SnapshotLoadResult LoadSnapshot(
      const persistence::Snapshot& snapshot);

  /**
   * @brief Replays persisted WAL operations into this store.
   *
   * @param wal Write-ahead log to replay.
   * @param offset WAL byte offset to start replaying from.
   * @return Number of WAL operations applied.
   */
  std::size_t ReplayFromWal(const persistence::WriteAheadLog& wal,
                            std::uint64_t offset = 0);

 private:
  /** Serializes administrative operations; foreground calls use shard locks. */
  mutable std::shared_mutex mutex_;
  static constexpr std::size_t kShardCount = 64;
  struct alignas(64) Shard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::string> data;
  };
  std::array<Shard, kShardCount> shards_;
  std::vector<std::unique_lock<std::shared_mutex>> LockAllShardsExclusive();
  std::vector<std::shared_lock<std::shared_mutex>> LockAllShardsShared() const;
  /** @brief Optional WAL used to persist future mutations. */
  persistence::WriteAheadLog* wal_ = nullptr;
  /** @brief Optional snapshot writer used to checkpoint the in-memory map. */
  persistence::Snapshot* snapshot_ = nullptr;
  /** @brief Number of write commands applied since the last snapshot. */
  std::atomic<std::size_t> writes_since_snapshot_{0};
  std::mutex snapshot_mutex_;
  std::mutex checkpoint_mutex_;
  std::condition_variable checkpoint_ready_;
  std::thread checkpoint_thread_;
  bool checkpoint_requested_ = false;
  bool checkpoint_active_ = false;
  bool checkpoint_stopping_ = false;
  std::uint64_t checkpoint_epoch_ = 0;
  std::exception_ptr checkpoint_error_;

  static std::size_t ShardIndex(const std::string& key);
  std::unordered_map<std::string, std::string> CollectDataLocked() const;
  void ReplaceDataLocked(std::unordered_map<std::string, std::string> data);

  static constexpr std::size_t kSnapshotInterval = 1000;
  void CheckCheckpointError();
  void ScheduleCheckpoint();
  void CheckpointLoop();
  void CancelPendingCheckpoint();
  /** @brief Writes a verified snapshot. Caller must hold mutex_ exclusively. */
  bool SaveSnapshotLocked();
  /**
   * @brief Writes a verified compacted snapshot and rotates WAL.
   *
   * Caller must hold mutex_ exclusively.
   */
  bool CompactPersistenceLocked();
};

}  // namespace store
}  // namespace kv

#endif  // KV_STORE_STORE_KV_STORE_H_
