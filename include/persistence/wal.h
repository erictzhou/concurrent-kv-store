#ifndef KV_STORE_WAL_H_
#define KV_STORE_WAL_H_

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kv {
namespace persistence {

enum class DurabilityPolicy { Buffered, Sync, GroupCommit };

enum class WalFaultPoint {
  AfterWriteBeforeSync,
  AfterSync,
  BeforeTruncate,
};

using WalFaultHook = std::function<void(WalFaultPoint)>;

struct WalStats {
  std::uint64_t records = 0;
  std::uint64_t sync_calls = 0;
  std::vector<std::uint64_t> batch_histogram;
};

enum class WalReplayStatus {
  CleanEof,
  InvalidHeader,
  PartialRecord,
  InvalidLength,
  InvalidSequence,
  InvalidOpcode,
  ChecksumMismatch,
  PartialPayload,
  InvalidPayload,
};

struct WalReplayResult {
  std::size_t applied_operations = 0;
  WalReplayStatus status = WalReplayStatus::CleanEof;
  std::uint64_t start_offset = 0;
  std::uint64_t last_good_offset = 0;
  std::uint64_t stop_offset = 0;
  std::uint64_t generation = 0;
  std::uint64_t last_sequence = 0;
  bool truncated = false;
};

/**
 * @brief Append-only write-ahead log for SET and DELETE operations.
 *
 * New WAL files use portable little-endian v3. A 24-byte file header carries
 * magic, version, byte-order marker, generation, and CRC32. Each frame is:
 *
 *   [le32 record_length][le32 crc32(record)][le64 sequence][payload bytes]
 *
 * The payload is one of:
 *
 *   SET:    [uint8 op=1][uint32 key_size][key][uint32 value_size][value]
 *   DELETE: [uint8 op=2][uint32 key_size][key]
 *
 * Lengths in the payload are also little-endian. Sequence numbers start at
 * one within each generation and follow the physical WAL write order. Replay
 * validates the header, frame, checksum, and sequence before mutation. Legacy
 * v2 logs remain readable and appendable until rotation upgrades them.
 *
 * Buffered acknowledges after userspace flush into the kernel page cache.
 * Sync calls fdatasync for each record; GroupCommit batches records and calls
 * fdatasync once per batch. GroupCommit acknowledgement waits for its batch.
 */
class WriteAheadLog {
 public:
  /**
   * @brief Opens the WAL file in append mode.
   *
   * @param path Path to the WAL file.
   */
  explicit WriteAheadLog(std::string path = "kv_store.wal",
                         DurabilityPolicy policy = DurabilityPolicy::Buffered,
                         std::size_t max_batch = 32,
                         std::uint32_t max_delay_us = 20,
                         WalFaultHook fault_hook = {});
  ~WriteAheadLog();

  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;

  WalStats GetStats() const;
  void ResetStats();
  void PinWriterToCpu(int cpu);

  /**
   * @brief Appends and flushes a SET record.
   *
   * @param key Key being written.
   * @param value Value being written.
   */
  void AppendSet(const std::string& key, const std::string& value);

  /**
   * @brief Appends and flushes a DELETE record.
   *
   * @param key Key being deleted.
   */
  void AppendDelete(const std::string& key);

  /**
   * @brief Returns the current flushed end offset of the WAL file.
   *
   * The stream is flushed before reporting the offset so snapshots can record a
   * byte position that includes all WAL records written so far.
   *
   * @return Current byte offset from the beginning of the WAL file.
   */
  std::uint64_t CurrentOffset();

  /**
   * @brief Truncates the WAL file and reopens it for future append records.
   *
   * This clears durable history without changing any in-memory store state.
   */
  void Clear();

  /**
   * @brief Rotates the WAL to a new empty log for future append records.
   *
   * Snapshot compaction uses rotation after a verified snapshot covers the
   * current in-memory state. Recovery then loads the snapshot and replays the
   * new WAL from offset zero.
   */
  void Rotate();

  /**
   * @brief Truncates the WAL to an already-validated record boundary.
   *
   * @param offset Byte offset to keep through.
   * @throws std::runtime_error if truncation fails or offset is past EOF.
   */
  void TruncateTo(std::uint64_t offset);

  /**
   * @brief Replays valid WAL records into an in-memory map.
   *
   * Replay stops at EOF or at the first malformed, corrupt, or incomplete
   * record. The bad record is not applied.
   *
   * @param store Store map to update while replaying the log.
   * @return Number of valid operations applied.
   */
  std::size_t Replay(std::unordered_map<std::string, std::string>& store) const;

  /**
   * @brief Replays WAL records and returns detailed stop/recovery status.
   *
   * @param store Store map to update while replaying the log.
   * @return Detailed replay result including last known-good offset.
   */
  WalReplayResult ReplayDetailed(
      std::unordered_map<std::string, std::string>& store) const;

  /**
   * @brief Replays valid WAL records starting at a byte offset.
   *
   * This is used with snapshots: the snapshot stores the WAL offset it covers,
   * and recovery replays only records written after that point.
   *
   * @param offset Byte offset to start replay from.
   * @param store Store map to update while replaying the log.
   * @return Number of valid operations applied.
   */
  std::size_t ReplayFrom(
      std::uint64_t offset,
      std::unordered_map<std::string, std::string>& store) const;

  /**
   * @brief Replays WAL records from an offset and returns detailed status.
   *
   * Replay stops at the first malformed, corrupt, or incomplete record. Records
   * after that point are not trusted because the validated prefix has ended.
   *
   * @param offset Byte offset to start replay from.
   * @param store Store map to update while replaying the log.
   * @return Detailed replay result including last known-good offset.
   */
  WalReplayResult ReplayFromDetailed(
      std::uint64_t offset,
      std::unordered_map<std::string, std::string>& store) const;

  /**
   * @brief Replays from an offset and truncates any corrupt tail.
   *
   * If replay stops before clean EOF, the WAL is truncated to the last
   * validated record boundary. This should be used only during recovery when
   * the suffix after the first bad frame is considered an untrusted crash tail.
   *
   * @param offset Byte offset to start replay from.
   * @param store Store map to update while replaying the log.
   * @return Detailed replay result, with truncated set when truncation happened.
   */
  WalReplayResult ReplayFromAndTruncate(
      std::uint64_t offset,
      std::unordered_map<std::string, std::string>& store);

 private:
  /** @brief Filesystem path of the WAL file. */
  std::string path_;
  /** @brief Append stream kept open for write path operations. */
  std::ofstream output_;
  DurabilityPolicy policy_;
  std::size_t max_batch_;
  std::uint32_t max_delay_us_;
  int sync_fd_ = -1;
  bool legacy_v2_ = false;
  bool header_pending_ = false;
  bool format_valid_ = true;
  bool needs_recovery_ = false;
  std::uint64_t generation_ = 1;
  std::uint64_t next_sequence_ = 1;
  WalFaultHook fault_hook_;
  mutable std::mutex io_mutex_;
  WalStats stats_;
  struct Pending {
    std::string frame;
    std::mutex mutex;
    std::condition_variable done;
    bool complete = false;
    std::exception_ptr error;
  };
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::deque<std::shared_ptr<Pending>> queue_;
  std::thread writer_;
  bool stopping_ = false;
  std::exception_ptr failure_;

  void AppendPayload(std::string payload);
  std::string PrepareFrame(const std::string& payload) const;
  void FinalizeFrameLocked(std::string& frame);
  void WriteFrameLocked(std::string& frame);
  void EnsureHeaderLocked();
  void SyncLocked();
  void WriterLoop();
};

}  // namespace persistence
}  // namespace kv

#endif  // KV_STORE_WAL_H_
