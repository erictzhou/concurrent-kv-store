#include "persistence/wal.h"
#include "store/kv_store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <barrier>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "helpers/file_utils.h"
#include "helpers/temp_dir.h"

namespace {

using kv::persistence::WalReplayStatus;
using kv::persistence::DurabilityPolicy;
using kv::persistence::WriteAheadLog;
using kv::persistence::WalFaultPoint;
using kv::store::KVStore;
using kv::tests::AppendBinaryFile;
using kv::tests::AppendPrimitive;
using kv::tests::FileExists;
using kv::tests::FileSize;
using kv::tests::ReadBinaryFile;
using kv::tests::RemoveIfExists;
using kv::tests::TempDir;
using kv::tests::WriteBinaryFile;

constexpr std::uint8_t kSetOp = 1;
constexpr std::uint8_t kDeleteOp = 2;
constexpr std::size_t kMaxWalRecordLength = 64U * 1024U * 1024U;

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

template <typename T>
void AppendLittle(std::string& bytes, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes.push_back(static_cast<char>(value >> (8 * i)));
  }
}

std::string LegacyFrameRecord(const std::string& payload) {
  std::string bytes;
  AppendPrimitive<std::uint32_t>(bytes,
                                 static_cast<std::uint32_t>(payload.size()));
  AppendPrimitive<std::uint32_t>(bytes, Crc32(payload));
  bytes.append(payload);
  return bytes;
}

std::string FrameRecord(const std::string& payload,
                        std::uint64_t sequence = 2) {
  std::string record;
  AppendLittle(record, sequence);
  record.append(payload);
  std::string bytes;
  AppendLittle(bytes, static_cast<std::uint32_t>(record.size()));
  AppendLittle(bytes, Crc32(record));
  bytes.append(record);
  return bytes;
}

template <typename T>
T ReadLittle(const std::string& bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
    throw std::runtime_error("short test frame");
  }
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(static_cast<unsigned char>(bytes[offset + i]))
             << (8 * i);
  }
  return value;
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

std::string DeletePayload(const std::string& key) {
  std::string payload;
  AppendPrimitive<std::uint8_t>(payload, kDeleteOp);
  AppendPrimitive<std::uint32_t>(payload, static_cast<std::uint32_t>(key.size()));
  payload.append(key);
  return payload;
}

std::string BadOpcodePayload() {
  std::string payload;
  AppendPrimitive<std::uint8_t>(payload, static_cast<std::uint8_t>(99));
  AppendPrimitive<std::uint32_t>(payload, 0U);
  return payload;
}

std::string CorruptPayloadByte(std::string frame) {
  constexpr std::size_t kPayloadStart = sizeof(std::uint32_t) * 2;
  frame.at(kPayloadStart) ^= 0x01;
  return frame;
}

class WalTest : public ::testing::Test {
 protected:
  WalTest() : wal_path_(temp_dir_.FilePath("wal.log")) {}

  std::unordered_map<std::string, std::string> Replay() const {
    WriteAheadLog wal(wal_path_);
    std::unordered_map<std::string, std::string> recovered;
    wal.Replay(recovered);
    return recovered;
  }

  TempDir temp_dir_;
  std::string wal_path_;
};

TEST_F(WalTest, SyncPolicySynchronizesEachAcknowledgedMutation) {
  WriteAheadLog wal(wal_path_, DurabilityPolicy::Sync);
  wal.AppendSet("a", "1");
  wal.AppendSet("b", "2");
  wal.AppendDelete("a");
  const auto stats = wal.GetStats();
  EXPECT_EQ(3U, stats.records);
  EXPECT_EQ(3U, stats.sync_calls);
  std::unordered_map<std::string, std::string> recovered;
  EXPECT_EQ(3U, wal.Replay(recovered));
  EXPECT_EQ("2", recovered.at("b"));
  EXPECT_EQ(0U, recovered.count("a"));
}

TEST_F(WalTest, GroupCommitSharesSyncAndReplaysEveryAcknowledgedMutation) {
  constexpr int kWriters = 8;
  WriteAheadLog wal(wal_path_, DurabilityPolicy::GroupCommit, kWriters, 5000);
  std::barrier ready(kWriters);
  std::vector<std::thread> writers;
  for (int i = 0; i < kWriters; ++i) {
    writers.emplace_back([&, i] {
      ready.arrive_and_wait();
      wal.AppendSet("key-" + std::to_string(i), "value-" + std::to_string(i));
    });
  }
  for (auto& writer : writers) writer.join();
  const auto stats = wal.GetStats();
  EXPECT_EQ(kWriters, stats.records);
  EXPECT_LT(stats.sync_calls, kWriters);
  std::unordered_map<std::string, std::string> recovered;
  EXPECT_EQ(kWriters, wal.Replay(recovered));
  EXPECT_EQ(kWriters, wal.ReplayDetailed(recovered).last_sequence);
  for (int i = 0; i < kWriters; ++i) {
    EXPECT_EQ("value-" + std::to_string(i), recovered.at("key-" + std::to_string(i)));
  }
}

TEST_F(WalTest, SyncFaultsBeforeAndAfterFdatasyncNeverMutateMemory) {
  for (const auto point : {WalFaultPoint::AfterWriteBeforeSync,
                           WalFaultPoint::AfterSync}) {
    const auto path = temp_dir_.FilePath(
        point == WalFaultPoint::AfterSync ? "after-sync.wal" : "before-sync.wal");
    {
      WriteAheadLog wal(path, DurabilityPolicy::Sync, 32, 20,
                        [point](WalFaultPoint hit) {
                          if (hit == point) throw std::runtime_error("injected WAL fault");
                        });
      KVStore store(&wal);
      EXPECT_THROW(store.Set("uncertain", "value"), std::runtime_error);
      EXPECT_FALSE(store.Contains("uncertain"));
      EXPECT_THROW(store.Set("later", "value"), std::runtime_error);
    }
    WriteAheadLog recovered_wal(path);
    std::unordered_map<std::string, std::string> recovered;
    const auto result = recovered_wal.ReplayDetailed(recovered);
    EXPECT_EQ(WalReplayStatus::CleanEof, result.status);
    EXPECT_EQ(1U, result.last_sequence);
    EXPECT_EQ("value", recovered.at("uncertain"));
    EXPECT_EQ(0U, recovered.count("later"));
  }
}

TEST_F(WalTest, GroupCommitFaultFailsEveryQueuedWaiter) {
  constexpr int kWriters = 8;
  WriteAheadLog wal(wal_path_, DurabilityPolicy::GroupCommit, kWriters, 5000,
                    [](WalFaultPoint point) {
                      if (point == WalFaultPoint::AfterWriteBeforeSync) {
                        throw std::runtime_error("injected group write fault");
                      }
                    });
  KVStore store(&wal);
  std::barrier ready(kWriters);
  std::vector<std::thread> writers;
  std::vector<int> failed(kWriters);
  for (int i = 0; i < kWriters; ++i) {
    writers.emplace_back([&, i] {
      ready.arrive_and_wait();
      try {
        store.Set("key-" + std::to_string(i), "value");
      } catch (const std::runtime_error&) {
        failed[i] = true;
      }
    });
  }
  for (auto& writer : writers) writer.join();
  for (const bool value : failed) EXPECT_TRUE(value);
  EXPECT_EQ(0U, store.Size());
}

TEST_F(WalTest, NewWalUsesLittleEndianV3HeaderAndOrderedSequences) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("first", "1");
    wal.AppendSet("second", "2");
  }
  const auto bytes = ReadBinaryFile(wal_path_);
  ASSERT_GE(bytes.size(), 24U);
  EXPECT_EQ("KVW3", bytes.substr(0, 4));
  EXPECT_EQ(3U, ReadLittle<std::uint32_t>(bytes, 4));
  EXPECT_EQ(0x01020304U, ReadLittle<std::uint32_t>(bytes, 8));
  EXPECT_EQ(1U, ReadLittle<std::uint64_t>(bytes, 12));
  const auto first_length = ReadLittle<std::uint32_t>(bytes, 24);
  EXPECT_EQ(1U, ReadLittle<std::uint64_t>(bytes, 32));
  const std::size_t second_offset = 24 + 8 + first_length;
  ASSERT_GE(bytes.size(), second_offset + 16);
  EXPECT_EQ(2U, ReadLittle<std::uint64_t>(bytes, second_offset + 8));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);
  EXPECT_EQ(WalReplayStatus::CleanEof, result.status);
  EXPECT_EQ(1U, result.generation);
  EXPECT_EQ(2U, result.last_sequence);
  EXPECT_EQ("2", recovered.at("second"));
}

TEST_F(WalTest, LegacyV2ReplaysAndRotationUpgradesToV3) {
  WriteBinaryFile(wal_path_, LegacyFrameRecord(SetPayload("old", "1")));
  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  EXPECT_EQ(1U, wal.Replay(recovered));
  wal.AppendSet("legacy-tail", "2");
  EXPECT_EQ(2U, wal.Replay(recovered));
  wal.Rotate();
  EXPECT_EQ(0U, FileSize(wal_path_));
  wal.AppendSet("new", "3");
  const auto bytes = ReadBinaryFile(wal_path_);
  EXPECT_EQ("KVW3", bytes.substr(0, 4));
  EXPECT_EQ(1U, ReadLittle<std::uint64_t>(bytes, 12));
  recovered.clear();
  EXPECT_EQ(1U, wal.Replay(recovered));
  EXPECT_EQ(1U, recovered.size());
  EXPECT_EQ("3", recovered.at("new"));
}

TEST_F(WalTest, PartialV3HeaderIsRejectedBeforeReplayOrAppend) {
  WriteBinaryFile(wal_path_, "KVW3\x03\x00");
  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  EXPECT_EQ(WalReplayStatus::InvalidHeader, wal.ReplayDetailed(recovered).status);
  EXPECT_THROW(wal.AppendSet("bad", "value"), std::runtime_error);
}

TEST_F(WalTest, CorruptedV3HeaderChecksumIsRejected) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }
  std::string bytes = ReadBinaryFile(wal_path_);
  bytes[20] ^= 0x01;
  WriteBinaryFile(wal_path_, bytes);
  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  EXPECT_EQ(WalReplayStatus::InvalidHeader, wal.ReplayDetailed(recovered).status);
  EXPECT_TRUE(recovered.empty());
  EXPECT_THROW(wal.AppendSet("bad", "value"), std::runtime_error);
}

TEST_F(WalTest, SequenceContinuesAfterReopenAndGenerationChangesOnRotate) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("first", "1");
  }
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("second", "2");
    std::unordered_map<std::string, std::string> recovered;
    EXPECT_EQ(2U, wal.ReplayDetailed(recovered).last_sequence);
    wal.Rotate();
    wal.AppendSet("third", "3");
    recovered.clear();
    const auto result = wal.ReplayDetailed(recovered);
    EXPECT_EQ(2U, result.generation);
    EXPECT_EQ(1U, result.last_sequence);
    EXPECT_EQ(1U, recovered.size());
  }
}

TEST_F(WalTest, InvalidSequenceStopsReplayAndCanBeTruncated) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "1");
    wal.AppendSet("bad", "2");
  }
  std::string bytes = ReadBinaryFile(wal_path_);
  const std::size_t second = 24 + 8 + ReadLittle<std::uint32_t>(bytes, 24);
  const auto second_length = ReadLittle<std::uint32_t>(bytes, second);
  ASSERT_GE(bytes.size(), second + 8 + second_length);
  bytes[second + 8] = 9;
  const std::string record = bytes.substr(second + 8, second_length);
  const auto checksum = Crc32(record);
  for (std::size_t i = 0; i < sizeof(checksum); ++i) {
    bytes[second + 4 + i] = static_cast<char>(checksum >> (8 * i));
  }
  WriteBinaryFile(wal_path_, bytes);

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);
  EXPECT_EQ(WalReplayStatus::InvalidSequence, result.status);
  EXPECT_EQ(1U, result.last_sequence);
  EXPECT_EQ(1U, result.applied_operations);
  EXPECT_THROW(wal.AppendSet("later", "3"), std::runtime_error);
  EXPECT_TRUE(wal.ReplayFromAndTruncate(0, recovered).truncated);
  wal.AppendSet("later", "3");
  recovered.clear();
  EXPECT_EQ(2U, wal.Replay(recovered));
  EXPECT_EQ("3", recovered.at("later"));
}

TEST_F(WalTest, ConstructorCreatesEmptyWalFile) {
  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;

  EXPECT_TRUE(FileExists(wal_path_));
  EXPECT_EQ(0U, FileSize(wal_path_));
  EXPECT_EQ(0U, wal.Replay(recovered));
  EXPECT_TRUE(recovered.empty());
}

TEST_F(WalTest, MissingWalPathReplaysAsNoOp) {
  WriteAheadLog wal(wal_path_);
  RemoveIfExists(wal_path_);
  std::unordered_map<std::string, std::string> recovered{{"keep", "value"}};

  EXPECT_EQ(0U, wal.Replay(recovered));
  EXPECT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered["keep"]);
}

TEST_F(WalTest, AppendingPutRecordRestoresStateAfterReplay) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("alpha", "1");
  }

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("alpha"));
}

TEST_F(WalTest, AppendingDeleteRecordRemovesKeyAfterReplay) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("alpha", "1");
    wal.AppendDelete("alpha");
  }

  const auto recovered = Replay();

  EXPECT_TRUE(recovered.empty());
}

TEST_F(WalTest, MultipleOperationsOnSameKeyReplayFinalState) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("hot", "1");
    wal.AppendSet("hot", "2");
    wal.AppendDelete("hot");
    wal.AppendSet("hot", "3");
  }

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("3", recovered.at("hot"));
}

TEST_F(WalTest, InterleavedOperationsAcrossKeysPreserveFinalState) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("a", "1");
    wal.AppendSet("b", "1");
    wal.AppendDelete("a");
    wal.AppendSet("c", "1");
    wal.AppendSet("b", "2");
  }

  const auto recovered = Replay();

  ASSERT_EQ(2U, recovered.size());
  EXPECT_EQ("2", recovered.at("b"));
  EXPECT_EQ("1", recovered.at("c"));
  EXPECT_EQ(recovered.end(), recovered.find("a"));
}

TEST_F(WalTest, ReplayHandlesManyRecords) {
  {
    WriteAheadLog wal(wal_path_);
    for (int i = 0; i < 5000; ++i) {
      wal.AppendSet("key-" + std::to_string(i), "value-" + std::to_string(i));
    }
  }

  const auto recovered = Replay();

  ASSERT_EQ(5000U, recovered.size());
  EXPECT_EQ("value-0", recovered.at("key-0"));
  EXPECT_EQ("value-4999", recovered.at("key-4999"));
}

TEST_F(WalTest, ZeroLengthKeyAndValueRoundTrip) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("", "");
  }

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  ASSERT_NE(recovered.end(), recovered.find(""));
  EXPECT_EQ("", recovered.at(""));
}

TEST_F(WalTest, LargeRecordReplay) {
  const std::string large_value(512 * 1024, 'v');
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("large", large_value);
  }

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ(large_value, recovered.at("large"));
}

TEST_F(WalTest, ValidWalReplayReportsCleanEofAndVerifiesChecksum) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("alpha", "1");
    wal.AppendDelete("missing");
  }

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::CleanEof, result.status);
  EXPECT_EQ(2U, result.applied_operations);
  EXPECT_EQ(FileSize(wal_path_), result.last_good_offset);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("alpha"));
}

TEST_F(WalTest, ReplayFromOffsetStartsAtRecordBoundary) {
  std::uint64_t offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("old", "skip");
    offset = wal.CurrentOffset();
    wal.AppendSet("new", "apply");
  }

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;

  EXPECT_EQ(1U, wal.ReplayFrom(offset, recovered));
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("apply", recovered.at("new"));
  EXPECT_EQ(recovered.end(), recovered.find("old"));
}

TEST_F(WalTest, BadOpcodeStopsReplayAndDoesNotApplyRecord) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("before", "1");
  }
  AppendBinaryFile(wal_path_, FrameRecord(BadOpcodePayload()));
  AppendBinaryFile(wal_path_, FrameRecord(SetPayload("after", "ignored")));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::InvalidOpcode, result.status);
  EXPECT_EQ(1U, result.applied_operations);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("before"));
  EXPECT_EQ(recovered.end(), recovered.find("after"));
}

TEST_F(WalTest, PartialKeyPayloadStopsWithoutApplyingRecord) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("before", "1");
  }

  std::string malformed;
  AppendPrimitive<std::uint8_t>(malformed, kSetOp);
  AppendPrimitive<std::uint32_t>(malformed, 100U);
  malformed.append("short");
  AppendBinaryFile(wal_path_, FrameRecord(malformed));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::PartialPayload, result.status);
  EXPECT_EQ(1U, result.applied_operations);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("before"));
}

TEST_F(WalTest, PartialValuePayloadStopsWithoutApplyingRecord) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("before", "1");
  }

  std::string malformed;
  AppendPrimitive<std::uint8_t>(malformed, kSetOp);
  AppendPrimitive<std::uint32_t>(malformed, 3U);
  malformed.append("bad");
  AppendPrimitive<std::uint32_t>(malformed, 100U);
  malformed.append("short");
  AppendBinaryFile(wal_path_, FrameRecord(malformed));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::PartialPayload, result.status);
  EXPECT_EQ(1U, result.applied_operations);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("before"));
  EXPECT_EQ(recovered.end(), recovered.find("bad"));
}

TEST_F(WalTest, ExtraTrailingPayloadBytesStopsWithoutApplyingRecord) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("victim", "value");
  }

  std::string malformed = DeletePayload("victim");
  malformed.push_back('x');
  AppendBinaryFile(wal_path_, FrameRecord(malformed));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::InvalidPayload, result.status);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered.at("victim"));
}

TEST_F(WalTest, PartialTrailingLengthIsReportedSafely) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }

  std::string partial_length;
  AppendPrimitive<std::uint32_t>(partial_length, 10U);
  partial_length.resize(2);
  AppendBinaryFile(wal_path_, partial_length);

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::PartialRecord, result.status);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered.at("good"));
}

TEST_F(WalTest, TornWriteInMiddleOfRecordStopsAfterValidRecords) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }

  std::string torn_record = FrameRecord(SetPayload("torn", "ignored"));
  torn_record.resize(torn_record.size() - 3);
  AppendBinaryFile(wal_path_, torn_record);

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::PartialRecord, result.status);
  EXPECT_EQ(1U, result.applied_operations);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered.at("good"));
  EXPECT_EQ(recovered.end(), recovered.find("torn"));
}

TEST_F(WalTest, ImpossibleRecordLengthStopsWithoutAllocatingPayload) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }

  std::string impossible_record;
  AppendPrimitive<std::uint32_t>(
      impossible_record, static_cast<std::uint32_t>(kMaxWalRecordLength + 1));
  AppendBinaryFile(wal_path_, impossible_record);
  AppendBinaryFile(wal_path_, FrameRecord(SetPayload("after", "ignored")));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::InvalidLength, result.status);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered.at("good"));
  EXPECT_EQ(recovered.end(), recovered.find("after"));
}

TEST_F(WalTest, ChecksumMismatchStopsAndDoesNotApplyCorruptedRecord) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("before", "1");
  }
  AppendBinaryFile(
      wal_path_, CorruptPayloadByte(FrameRecord(SetPayload("bad", "value"))));

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayDetailed(recovered);

  EXPECT_EQ(WalReplayStatus::ChecksumMismatch, result.status);
  EXPECT_EQ(1U, result.applied_operations);
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("1", recovered.at("before"));
  EXPECT_EQ(recovered.end(), recovered.find("bad"));
}

TEST_F(WalTest, SafeTruncateAfterCorruptedTailKeepsOnlyValidatedPrefix) {
  std::uint64_t good_offset = 0;
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
    good_offset = wal.CurrentOffset();
  }
  AppendBinaryFile(
      wal_path_, CorruptPayloadByte(FrameRecord(SetPayload("bad", "value"))));
  ASSERT_GT(FileSize(wal_path_), good_offset);

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayFromAndTruncate(0, recovered);

  EXPECT_EQ(WalReplayStatus::ChecksumMismatch, result.status);
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(good_offset, result.last_good_offset);
  EXPECT_EQ(good_offset, FileSize(wal_path_));
  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("value", recovered.at("good"));
}

TEST_F(WalTest, TruncatedWalCanAppendAndReplayNewRecords) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }
  AppendBinaryFile(
      wal_path_, CorruptPayloadByte(FrameRecord(SetPayload("bad", "x"))));

  {
    WriteAheadLog wal(wal_path_);
    std::unordered_map<std::string, std::string> recovered;
    ASSERT_TRUE(wal.ReplayFromAndTruncate(0, recovered).truncated);
    wal.AppendSet("after", "2");
  }

  const auto recovered = Replay();

  ASSERT_EQ(2U, recovered.size());
  EXPECT_EQ("value", recovered.at("good"));
  EXPECT_EQ("2", recovered.at("after"));
}

TEST_F(WalTest, OffsetPastEndReplaysNoRecords) {
  {
    WriteAheadLog wal(wal_path_);
    wal.AppendSet("good", "value");
  }

  WriteAheadLog wal(wal_path_);
  std::unordered_map<std::string, std::string> recovered;
  const auto result = wal.ReplayFromDetailed(1000000, recovered);

  EXPECT_EQ(WalReplayStatus::CleanEof, result.status);
  EXPECT_EQ(0U, result.applied_operations);
  EXPECT_TRUE(recovered.empty());
}

TEST_F(WalTest, ManuallyWrittenChecksumFrameReplays) {
  WriteBinaryFile(wal_path_, LegacyFrameRecord(SetPayload("manual", "ok")));

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("ok", recovered.at("manual"));
  EXPECT_FALSE(ReadBinaryFile(wal_path_).empty());
}

}  // namespace
