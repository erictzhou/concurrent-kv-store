#include "persistence/wal.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <barrier>
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

std::string FrameRecord(const std::string& payload) {
  std::string bytes;
  AppendPrimitive<std::uint32_t>(bytes,
                                 static_cast<std::uint32_t>(payload.size()));
  AppendPrimitive<std::uint32_t>(bytes, Crc32(payload));
  bytes.append(payload);
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
  for (int i = 0; i < kWriters; ++i) {
    EXPECT_EQ("value-" + std::to_string(i), recovered.at("key-" + std::to_string(i)));
  }
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
  WriteBinaryFile(wal_path_, FrameRecord(SetPayload("manual", "ok")));

  const auto recovered = Replay();

  ASSERT_EQ(1U, recovered.size());
  EXPECT_EQ("ok", recovered.at("manual"));
  EXPECT_FALSE(ReadBinaryFile(wal_path_).empty());
}

}  // namespace
