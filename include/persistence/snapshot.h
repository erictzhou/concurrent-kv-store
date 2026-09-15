#ifndef KV_STORE_PERSISTENCE_SNAPSHOT_H_
#define KV_STORE_PERSISTENCE_SNAPSHOT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace kv {
namespace persistence {

/**
 * @brief Metadata returned after loading a snapshot.
 */
struct SnapshotLoadResult {
  /** @brief Whether a snapshot file was present and loaded. */
  bool loaded = false;
  /** @brief Number of key-value pairs loaded from the snapshot. */
  std::size_t entry_count = 0;
  /** @brief WAL byte offset covered by the loaded snapshot. */
  std::uint64_t wal_offset = 0;
};

enum class SnapshotFaultPoint {
  AfterTempWrite,
  AfterTempSync,
  AfterRename,
  AfterDirectorySync,
};

using SnapshotFaultHook = std::function<void(SnapshotFaultPoint)>;

/**
 * @brief Point-in-time snapshot storage for the in-memory key-value map.
 *
 * Snapshots persist the complete current state of the store. They are separate
 * from the WAL: a snapshot represents materialized data, while the WAL
 * represents ordered mutations that may need to be replayed after the snapshot.
 */
class Snapshot {
 public:
  /**
   * @brief Creates a snapshot handle for the configured file path.
   *
   * @param path Path to the snapshot file.
   */
  explicit Snapshot(std::string path = "kv_store.snapshot",
                    SnapshotFaultHook fault_hook = {});

  /**
   * @brief Writes the full contents of a store map to the snapshot file.
   *
   * Existing snapshot contents are replaced atomically by the implementation.
   *
   * @param store In-memory key-value data to persist.
   * @param wal_offset WAL byte offset covered by this snapshot.
   */
  void Save(const std::unordered_map<std::string, std::string>& store,
            std::uint64_t wal_offset = 0) const;

  /**
   * @brief Writes and verifies a snapshot before returning.
   *
   * Verification reloads the committed snapshot and checks that its entry
   * count, covered WAL offset, and key/value contents match the expected
   * in-memory state. Callers that plan to delete or rotate WAL history should
   * use this method instead of `Save()`.
   *
   * @param store In-memory key-value data to persist.
   * @param wal_offset WAL byte offset covered by this snapshot.
   * @throws std::runtime_error if writing or verification fails.
   */
  void SaveVerified(const std::unordered_map<std::string, std::string>& store,
                    std::uint64_t wal_offset = 0) const;

  /**
   * @brief Verifies the committed snapshot matches expected state and metadata.
   *
   * @param expected_store Expected key-value data.
   * @param expected_wal_offset Expected covered WAL byte offset.
   * @return `true` when the snapshot is present and matches exactly.
   */
  bool Verify(const std::unordered_map<std::string, std::string>& expected_store,
              std::uint64_t expected_wal_offset) const;

  /**
   * @brief Removes the current snapshot file and any temporary snapshot file.
   *
   * Missing files are treated as already cleared.
   */
  void Clear() const;

  /**
   * @brief Loads snapshot contents into a store map.
   *
   * The target map is replaced with the snapshot contents only after a valid
   * snapshot has been read.
   *
   * @param store In-memory map to populate.
   * @return Snapshot metadata including entry count and covered WAL offset.
   */
  SnapshotLoadResult Load(
      std::unordered_map<std::string, std::string>& store) const;

 private:
  /** @brief Filesystem path of the snapshot file. */
  std::string path_;
  SnapshotFaultHook fault_hook_;
};

}  // namespace persistence
}  // namespace kv

#endif  // KV_STORE_PERSISTENCE_SNAPSHOT_H_
