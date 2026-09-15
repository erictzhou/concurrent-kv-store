#include "persistence/snapshot.h"
#include "persistence/wal.h"
#include "store/kv_store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <array>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <sys/wait.h>
#include <unistd.h>

#include "helpers/file_utils.h"
#include "helpers/temp_dir.h"

namespace {

using kv::persistence::Snapshot;
using kv::persistence::SnapshotFaultPoint;
using kv::persistence::SnapshotLoadResult;
using kv::persistence::WriteAheadLog;
using kv::store::KVStore;
using kv::tests::AppendBinaryFile;
using kv::tests::AppendPrimitive;
using kv::tests::FileExists;
using kv::tests::FileSize;
using kv::tests::RemoveIfExists;
using kv::tests::TempDir;
using kv::tests::WriteBinaryFile;

constexpr std::uint32_t kSnapshotMagic = 0x3153564BU;
constexpr std::uint32_t kSnapshotVersion = 1;
constexpr std::uint8_t kSetOp = 1;

std::uint32_t Crc32(const std::string& bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

std::string FrameRecord(const std::string& payload) {
  std::string record;
  for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
    record.push_back(static_cast<char>(std::uint64_t{4} >> (8 * i)));
  }
  record.append(payload);
  std::string bytes;
  const auto length = static_cast<std::uint32_t>(record.size());
  const auto checksum = Crc32(record);
  for (std::size_t i = 0; i < sizeof(length); ++i) {
    bytes.push_back(static_cast<char>(length >> (8 * i)));
  }
  for (std::size_t i = 0; i < sizeof(checksum); ++i) {
    bytes.push_back(static_cast<char>(checksum >> (8 * i)));
  }
  bytes.append(record);
  return bytes;
}

std::string SetPayload(const std::string& key, const std::string& value) {
  std::string payload;
  AppendPrimitive<std::uint8_t>(payload, kSetOp);
  AppendPrimitive<std::uint32_t>(payload, static_cast<std::uint32_t>(key.size()));
  payload.append(key);
  AppendPrimitive<std::uint32_t>(payload,
                                 static_cast<std::uint32_t>(value.size()));
  payload.append(value);
  return payload;
}

std::string CorruptPayloadByte(std::string frame) {
  constexpr std::size_t kPayloadStart = sizeof(std::uint32_t) * 2;
  frame.at(kPayloadStart) ^= 0x01;
  return frame;
}

class RecoveryTest : public ::testing::Test {
 protected:
  RecoveryTest()
      : wal_path_(temp_dir_.FilePath("wal.log")),
        snapshot_path_(temp_dir_.FilePath("store.snapshot")) {}

  TempDir temp_dir_;
  std::string wal_path_;
  std::string snapshot_path_;
};

KVStore RecoverStore(const std::string& wal_path,
                     const std::string& snapshot_path) {
  WriteAheadLog wal(wal_path);
  Snapshot snapshot(snapshot_path);
  KVStore recovered;
  const SnapshotLoadResult snapshot_result = recovered.LoadSnapshot(snapshot);
  recovered.ReplayFromWal(wal, snapshot_result.wal_offset);
  return recovered;
}

TEST_F(RecoveryTest, WritesThenReconstructsStoreFromWal) {
  {
    WriteAheadLog wal(wal_path_);
    KVStore store(&wal);
    store.Set("alpha", "1");
    store.Set("message", "hello world");
    ASSERT_TRUE(store.Delete("alpha"));
  }

  WriteAheadLog wal(wal_path_);
  KVStore recovered(&wal);
  const std::size_t recovered_operations = recovered.ReplayFromWal(wal);

  EXPECT_EQ(3U, recovered_operations);
  EXPECT_EQ(1U, recovered.Size());
  EXPECT_FALSE(recovered.Contains("alpha"));
  ASSERT_TRUE(recovered.Get("message").has_value());
  EXPECT_EQ("hello world", recovered.Get("message").value());
}

TEST_F(RecoveryTest, PutDeleteOverwriteSequenceRecoversOnlyFinalState) {
  {
    WriteAheadLog wal(wal_path_);
    KVStore store(&wal);
    store.Set("a", "1");
    store.Set("b", "1");
    store.Set("a", "2");
    store.Delete("b");
    store.Set("c", "3");
    store.Delete("missing");
  }

  WriteAheadLog wal(wal_path_);
  KVStore recovered(&wal);

  EXPECT_EQ(6U, recovered.ReplayFromWal(wal));
  EXPECT_EQ(2U, recovered.Size());
  EXPECT_EQ("2", recovered.Get("a").value());
  EXPECT_EQ("3", recovered.Get("c").value());
  EXPECT_FALSE(recovered.Contains("b"));
  EXPECT_FALSE(recovered.Contains("missing"));
}

TEST_F(RecoveryTest, SnapshotOnlyRecoveryLoadsMaterializedState) {
  Snapshot snapshot(snapshot_path_);
  snapshot.Save({{"alpha", "1"}, {"empty", ""}}, 77);

  KVStore recovered;
  const SnapshotLoadResult result = recovered.LoadSnapshot(snapshot);

  EXPECT_TRUE(result.loaded);
  EXPECT_EQ(2U, result.entry_count);
  EXPECT_EQ(77U, result.wal_offset);
  EXPECT_EQ(2U, recovered.Size());
  EXPECT_EQ("1", recovered.Get("alpha").value());
  EXPECT_EQ("", recovered.Get("empty").value());
}

TEST_F(RecoveryTest, StoreCanSaveSnapshotThroughPublicApi) {
  std::uint64_t wal_offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("alpha", "1");
    store.Set("beta", "2");

    EXPECT_TRUE(store.SaveSnapshot());
    wal_offset = wal.CurrentOffset();
  }

  Snapshot snapshot(snapshot_path_);
  KVStore recovered;
  const SnapshotLoadResult result = recovered.LoadSnapshot(snapshot);

  EXPECT_TRUE(result.loaded);
  EXPECT_EQ(2U, result.entry_count);
  EXPECT_EQ(wal_offset, result.wal_offset);
  EXPECT_EQ("1", recovered.Get("alpha").value());
  EXPECT_EQ("2", recovered.Get("beta").value());
}

TEST_F(RecoveryTest, SnapshotPlusWalTailRecoversFromCoveredOffset) {
  std::uint64_t snapshot_offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("old-wal", "skip");
    snapshot_offset = wal.CurrentOffset();

    Snapshot snapshot(snapshot_path_);
    snapshot.Save({{"old", "snapshot"}, {"keep", "snapshot"}},
                  snapshot_offset);

    wal.AppendDelete("old");
    wal.AppendSet("keep", "wal");
    wal.AppendSet("new", "wal");
  }

  WriteAheadLog wal(wal_path_);
  Snapshot snapshot(snapshot_path_);
  KVStore recovered(&wal, &snapshot);
  const SnapshotLoadResult snapshot_result = recovered.LoadSnapshot(snapshot);
  const std::size_t wal_operations =
      recovered.ReplayFromWal(wal, snapshot_result.wal_offset);

  EXPECT_EQ(snapshot_offset, snapshot_result.wal_offset);
  EXPECT_EQ(3U, wal_operations);
  EXPECT_FALSE(recovered.Contains("old-wal"));
  EXPECT_FALSE(recovered.Contains("old"));
  EXPECT_EQ("wal", recovered.Get("keep").value());
  EXPECT_EQ("wal", recovered.Get("new").value());
}

TEST_F(RecoveryTest, SnapshotPlusWalTailStopsAtCorruptedChecksumTail) {
  std::uint64_t snapshot_offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("covered", "skip");
    snapshot_offset = wal.CurrentOffset();

    Snapshot snapshot(snapshot_path_);
    snapshot.Save({{"base", "snapshot"}, {"covered", "snapshot"}},
                  snapshot_offset);

    wal.AppendSet("base", "wal");
    wal.AppendSet("tail", "valid");
  }
  AppendBinaryFile(
      wal_path_, CorruptPayloadByte(FrameRecord(SetPayload("bad", "ignored"))));

  WriteAheadLog wal(wal_path_);
  Snapshot snapshot(snapshot_path_);
  KVStore recovered(&wal, &snapshot);
  const SnapshotLoadResult snapshot_result = recovered.LoadSnapshot(snapshot);
  const std::size_t wal_operations =
      recovered.ReplayFromWal(wal, snapshot_result.wal_offset);

  EXPECT_EQ(snapshot_offset, snapshot_result.wal_offset);
  EXPECT_EQ(2U, wal_operations);
  EXPECT_EQ("wal", recovered.Get("base").value());
  EXPECT_EQ("snapshot", recovered.Get("covered").value());
  EXPECT_EQ("valid", recovered.Get("tail").value());
  EXPECT_FALSE(recovered.Contains("bad"));
}

TEST_F(RecoveryTest, WalTailAfterSnapshotOverridesSnapshotValues) {
  std::uint64_t snapshot_offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    snapshot_offset = wal.CurrentOffset();
    snapshot.Save({{"key", "snapshot"}, {"delete-me", "snapshot"}},
                  snapshot_offset);
    wal.AppendSet("key", "wal");
    wal.AppendDelete("delete-me");
  }

  WriteAheadLog wal(wal_path_);
  Snapshot snapshot(snapshot_path_);
  KVStore recovered(&wal, &snapshot);
  const SnapshotLoadResult result = recovered.LoadSnapshot(snapshot);

  EXPECT_EQ(2U, recovered.ReplayFromWal(wal, result.wal_offset));
  EXPECT_EQ(1U, recovered.Size());
  EXPECT_EQ("wal", recovered.Get("key").value());
  EXPECT_FALSE(recovered.Contains("delete-me"));
}

TEST_F(RecoveryTest, MissingSnapshotFallsBackToFullWalReplay) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("alpha", "1");
    wal.AppendSet("beta", "2");
  }

  WriteAheadLog wal(wal_path_);
  Snapshot snapshot(snapshot_path_);
  KVStore recovered(&wal, &snapshot);
  const SnapshotLoadResult result = recovered.LoadSnapshot(snapshot);
  const std::size_t wal_operations =
      recovered.ReplayFromWal(wal, result.wal_offset);

  EXPECT_FALSE(result.loaded);
  EXPECT_EQ(0U, result.wal_offset);
  EXPECT_EQ(2U, wal_operations);
  EXPECT_EQ("1", recovered.Get("alpha").value());
  EXPECT_EQ("2", recovered.Get("beta").value());
}

TEST_F(RecoveryTest, CorruptedSnapshotThrowsWithoutReplacingLiveStore) {
  std::string bytes;
  AppendPrimitive<std::uint32_t>(bytes, kSnapshotMagic);
  AppendPrimitive<std::uint32_t>(bytes, kSnapshotVersion);
  AppendPrimitive<std::uint64_t>(bytes, 0U);
  AppendPrimitive<std::uint32_t>(bytes, 1U);
  WriteBinaryFile(snapshot_path_, bytes);

  Snapshot snapshot(snapshot_path_);
  KVStore recovered;
  recovered.Set("keep", "value");

  EXPECT_THROW(recovered.LoadSnapshot(snapshot), std::runtime_error);
  ASSERT_EQ(1U, recovered.Size());
  EXPECT_EQ("value", recovered.Get("keep").value());
}

TEST_F(RecoveryTest, StartupRecoveryIsIdempotentAcrossFreshStores) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("a", "1");
    wal.AppendSet("a", "2");
    wal.AppendSet("b", "3");
    wal.AppendDelete("b");
  }

  WriteAheadLog wal(wal_path_);
  KVStore first(&wal);
  KVStore second(&wal);

  EXPECT_EQ(4U, first.ReplayFromWal(wal));
  EXPECT_EQ(4U, second.ReplayFromWal(wal));
  EXPECT_EQ(first.Size(), second.Size());
  EXPECT_EQ(first.Get("a"), second.Get("a"));
  EXPECT_EQ(first.Get("b"), second.Get("b"));
}

TEST_F(RecoveryTest, ClearPersistenceKeepsMemoryButResetsDurableState) {
  Snapshot snapshot(snapshot_path_);
  snapshot.Save({{"from-snapshot", "old"}}, 0);

  {
    WriteAheadLog wal(wal_path_);
    KVStore store(&wal, &snapshot);
    store.Set("before-clear", "old");

    store.ClearPersistence();

    EXPECT_EQ("old", store.Get("before-clear").value());
    EXPECT_FALSE(FileExists(snapshot_path_));

    store.Set("after-clear", "new");
  }

  std::unordered_map<std::string, std::string> loaded_snapshot;
  const SnapshotLoadResult snapshot_result = snapshot.Load(loaded_snapshot);
  EXPECT_FALSE(snapshot_result.loaded);

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> replayed;
  EXPECT_EQ(1U, wal.Replay(replayed));
  ASSERT_EQ(1U, replayed.size());
  EXPECT_EQ("new", replayed.at("after-clear"));
  EXPECT_EQ(replayed.end(), replayed.find("before-clear"));
}

TEST_F(RecoveryTest, CompactPersistenceRotatesWalAndRecoversSnapshotOnly) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "1");
    store.Set("b", "2");
    ASSERT_GT(FileSize(wal_path_), 0U);

    ASSERT_TRUE(store.CompactPersistence());

    EXPECT_EQ(0U, FileSize(wal_path_));
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(2U, recovered.Size());
  EXPECT_EQ("1", recovered.Get("a").value());
  EXPECT_EQ("2", recovered.Get("b").value());
}

TEST_F(RecoveryTest, CompactedSnapshotPlusNewWalTailRecoversAllKeys) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "1");
    store.Set("b", "2");
    ASSERT_TRUE(store.CompactPersistence());
    store.Set("c", "3");
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(3U, recovered.Size());
  EXPECT_EQ("1", recovered.Get("a").value());
  EXPECT_EQ("2", recovered.Get("b").value());
  EXPECT_EQ("3", recovered.Get("c").value());
}

TEST_F(RecoveryTest, DeleteAfterCompactionRemovesSnapshotKeyOnRecovery) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "old");
    ASSERT_TRUE(store.CompactPersistence());
    EXPECT_TRUE(store.Delete("a"));
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(0U, recovered.Size());
  EXPECT_FALSE(recovered.Contains("a"));
}

TEST_F(RecoveryTest, OverwriteAfterCompactionWinsOnRecovery) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "old");
    ASSERT_TRUE(store.CompactPersistence());
    store.Set("a", "new");
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(1U, recovered.Size());
  EXPECT_EQ("new", recovered.Get("a").value());
}

TEST_F(RecoveryTest, FailedSnapshotWriteDoesNotRotateWal) {
  const std::string missing_dir_snapshot =
      temp_dir_.FilePath("missing-dir/store.snapshot");
  std::uintmax_t wal_size_before = 0;
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(missing_dir_snapshot);
    KVStore store(&wal, &snapshot);
    store.Set("a", "1");
    wal_size_before = FileSize(wal_path_);

    EXPECT_THROW(store.CompactPersistence(), std::runtime_error);
    EXPECT_EQ(wal_size_before, FileSize(wal_path_));
  }

  WriteAheadLog wal(wal_path_);
  KVStore recovered(&wal);

  EXPECT_EQ(1U, recovered.ReplayFromWal(wal));
  EXPECT_EQ("1", recovered.Get("a").value());
}

TEST_F(RecoveryTest, SnapshotPublicationFaultsLeaveWalRecoveryIntact) {
  const std::array points{
      SnapshotFaultPoint::AfterTempWrite,
      SnapshotFaultPoint::AfterTempSync,
      SnapshotFaultPoint::AfterRename,
      SnapshotFaultPoint::AfterDirectorySync,
  };
  for (std::size_t i = 0; i < points.size(); ++i) {
    const auto wal_path =
        temp_dir_.FilePath("publication-" + std::to_string(i) + ".wal");
    const auto snapshot_path =
        temp_dir_.FilePath("publication-" + std::to_string(i) + ".snapshot");
    Snapshot(snapshot_path).Save({{"base", "old"}}, 0);
    {
      WriteAheadLog wal(wal_path, kv::persistence::DurabilityPolicy::Sync);
      Snapshot faulting(snapshot_path, [point = points[i]](SnapshotFaultPoint hit) {
        if (hit == point) throw std::runtime_error("injected snapshot fault");
      });
      KVStore store(&wal, &faulting);
      store.Set("base", "new");
      store.Set("tail", "value");
      const auto wal_size = FileSize(wal_path);
      EXPECT_THROW(store.CompactPersistence(), std::runtime_error);
      EXPECT_EQ(wal_size, FileSize(wal_path));
    }
    const KVStore recovered = RecoverStore(wal_path, snapshot_path);
    EXPECT_EQ("new", recovered.Get("base").value());
    EXPECT_EQ("value", recovered.Get("tail").value());
  }
}

TEST_F(RecoveryTest, FaultBeforeWalRotationKeepsVerifiedSnapshotAndWal) {
  std::uintmax_t wal_size = 0;
  {
    WriteAheadLog wal(wal_path_, kv::persistence::DurabilityPolicy::Sync,
                      32, 20, [](kv::persistence::WalFaultPoint point) {
                        if (point == kv::persistence::WalFaultPoint::BeforeTruncate) {
                          throw std::runtime_error("injected WAL rotation fault");
                        }
                      });
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("base", "new");
    store.Set("tail", "value");
    wal_size = FileSize(wal_path_);
    EXPECT_THROW(store.CompactPersistence(), std::runtime_error);
    EXPECT_EQ(wal_size, FileSize(wal_path_));
  }
  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);
  EXPECT_EQ("new", recovered.Get("base").value());
  EXPECT_EQ("value", recovered.Get("tail").value());
}

TEST_F(RecoveryTest, CrashAfterSnapshotBeforeWalRotationStillRecovers) {
  {
    WriteAheadLog wal(wal_path_);
    KVStore store(&wal);
    store.Set("a", "old");
    store.Set("a", "new");

    Snapshot snapshot(snapshot_path_);
    snapshot.SaveVerified({{"a", "new"}}, 0);
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(1U, recovered.Size());
  EXPECT_EQ("new", recovered.Get("a").value());
  EXPECT_GT(FileSize(wal_path_), 0U);
}

TEST_F(RecoveryTest, AbruptExitAfterSyncBeforeMemoryMutationReplaysWal) {
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      WriteAheadLog wal(wal_path_, kv::persistence::DurabilityPolicy::Sync);
      wal.AppendSet("committed", "value");
      // Model a process dying after the WAL sync but before updating its map
      // or running destructors.
      ::_exit(0);
    } catch (...) {
      ::_exit(2);
    }
  }
  int status = 0;
  ASSERT_EQ(child, ::waitpid(child, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);
  EXPECT_EQ(kv::persistence::WalReplayStatus::CleanEof, result.status);
  EXPECT_EQ(1U, result.last_sequence);
  EXPECT_EQ("value", recovered.at("committed"));
}

TEST_F(RecoveryTest, AbandonedSnapshotTempDoesNotReplacePublishedSnapshot) {
  Snapshot snapshot(snapshot_path_);
  snapshot.Save({{"stable", "value"}}, 0);
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    WriteBinaryFile(snapshot_path_ + ".tmp", "partial snapshot");
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(child, ::waitpid(child, &status, 0));
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  std::unordered_map<std::string, std::string> recovered;
  const auto result = snapshot.Load(recovered);
  EXPECT_TRUE(result.loaded);
  EXPECT_EQ("value", recovered.at("stable"));
}

TEST_F(RecoveryTest, MissingWalAfterValidCompactedSnapshotRecoversSnapshot) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "1");
    ASSERT_TRUE(store.CompactPersistence());
  }
  RemoveIfExists(wal_path_);

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(1U, recovered.Size());
  EXPECT_EQ("1", recovered.Get("a").value());
}

TEST_F(RecoveryTest, CorruptedSnapshotDoesNotDestroyValidWalFallback) {
  {
    WriteAheadLog wal(wal_path_);
    KVStore store(&wal);
    store.Set("a", "1");
    store.Set("b", "2");
  }

  std::string bytes;
  AppendPrimitive<std::uint32_t>(bytes, kSnapshotMagic);
  AppendPrimitive<std::uint32_t>(bytes, 999U);
  WriteBinaryFile(snapshot_path_, bytes);
  const std::uintmax_t wal_size_before = FileSize(wal_path_);

  Snapshot snapshot(snapshot_path_);
  KVStore recovered_with_snapshot;
  EXPECT_THROW(recovered_with_snapshot.LoadSnapshot(snapshot),
               std::runtime_error);
  EXPECT_EQ(wal_size_before, FileSize(wal_path_));

  WriteAheadLog wal(wal_path_);
  KVStore recovered_from_wal(&wal);
  EXPECT_EQ(2U, recovered_from_wal.ReplayFromWal(wal));
  EXPECT_EQ("1", recovered_from_wal.Get("a").value());
  EXPECT_EQ("2", recovered_from_wal.Get("b").value());
}

TEST_F(RecoveryTest, RepeatedCompactionIsIdempotentAndKeepsLatestState) {
  {
    WriteAheadLog wal(wal_path_);
    Snapshot snapshot(snapshot_path_);
    KVStore store(&wal, &snapshot);
    store.Set("a", "1");
    ASSERT_TRUE(store.CompactPersistence());
    ASSERT_TRUE(store.CompactPersistence());
    store.Set("a", "2");
    store.Set("b", "3");
    ASSERT_TRUE(store.CompactPersistence());
    store.Set("c", "4");
  }

  const KVStore recovered = RecoverStore(wal_path_, snapshot_path_);

  EXPECT_EQ(3U, recovered.Size());
  EXPECT_EQ("2", recovered.Get("a").value());
  EXPECT_EQ("3", recovered.Get("b").value());
  EXPECT_EQ("4", recovered.Get("c").value());
}

TEST_F(RecoveryTest, RepeatedOpenCloseCyclesAppendAndReplayAllRecords) {
  for (int i = 0; i < 100; ++i) {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("key-" + std::to_string(i), "value-" + std::to_string(i));
  }

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;

  EXPECT_EQ(100U, wal.Replay(recovered));
  ASSERT_EQ(100U, recovered.size());
  EXPECT_EQ("value-0", recovered.at("key-0"));
  EXPECT_EQ("value-99", recovered.at("key-99"));
}

}  // namespace
