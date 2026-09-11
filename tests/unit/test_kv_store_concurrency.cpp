#include "persistence/snapshot.h"
#include "persistence/wal.h"
#include "store/kv_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "helpers/temp_dir.h"

namespace {

TEST(KVStoreCheckpointTest, AutomaticCheckpointCapturesOffsetAndReplaysLaterWrites) {
  kv::tests::TempDir temp_dir;
  const auto wal_path = temp_dir.FilePath("automatic.wal");
  const auto snapshot_path = temp_dir.FilePath("automatic.snapshot");
  {
    kv::persistence::WriteAheadLog wal(wal_path);
    kv::persistence::Snapshot snapshot(snapshot_path);
    kv::store::KVStore store(&wal, &snapshot);
    for (int i = 0; i < 1200; ++i) {
      store.Set("key-" + std::to_string(i), "value-" + std::to_string(i));
    }
    store.WaitForCheckpoints();
  }
  kv::persistence::WriteAheadLog wal(wal_path);
  kv::persistence::Snapshot snapshot(snapshot_path);
  kv::store::KVStore recovered;
  const auto loaded = recovered.LoadSnapshot(snapshot);
  ASSERT_TRUE(loaded.loaded);
  recovered.ReplayFromWal(wal, loaded.wal_offset);
  EXPECT_EQ(1200U, recovered.Size());
  EXPECT_EQ("value-1199", recovered.Get("key-1199").value());
}

TEST(KVStoreCheckpointTest, BackgroundFailureIsReported) {
  kv::tests::TempDir temp_dir;
  kv::persistence::Snapshot snapshot(temp_dir.FilePath("missing/checkpoint.snapshot"));
  kv::store::KVStore store(nullptr, &snapshot);
  for (int i = 0; i < 1000; ++i) {
    store.Set("key-" + std::to_string(i), "value");
  }
  EXPECT_THROW(store.WaitForCheckpoints(), std::runtime_error);
  EXPECT_THROW(store.Set("after-failure", "value"), std::runtime_error);
  EXPECT_FALSE(store.Contains("after-failure"));
}

using kv::persistence::Snapshot;
using kv::persistence::SnapshotLoadResult;
using kv::persistence::WriteAheadLog;
using kv::store::KVStore;
using kv::tests::TempDir;

void WaitForStart(const std::atomic<bool>& start) {
  while (!start.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

void WaitForReady(const std::atomic<int>& ready, int expected) {
  while (ready.load(std::memory_order_acquire) < expected) {
    std::this_thread::yield();
  }
}

std::string KeyFor(int thread_index, int key_index) {
  return "key-" + std::to_string(thread_index) + "-" +
         std::to_string(key_index);
}

std::string ValueFor(int thread_index, int key_index) {
  return "value-" + std::to_string(thread_index) + "-" +
         std::to_string(key_index);
}

void VerifyThreadKeyRange(const KVStore& store, int thread_index,
                          int keys_per_thread) {
  for (int key_index = 0; key_index < keys_per_thread; ++key_index) {
    const std::string key = KeyFor(thread_index, key_index);
    const std::string expected = ValueFor(thread_index, key_index);
    ASSERT_TRUE(store.Get(key).has_value()) << key;
    EXPECT_EQ(expected, store.Get(key).value()) << key;
  }
}

TEST(KVStoreConcurrencyTest, ConcurrentReadersSeeStableValues) {
  constexpr int kKeys = 256;
  constexpr int kReaders = 8;
  constexpr int kIterations = 3000;

  KVStore store;
  for (int key_index = 0; key_index < kKeys; ++key_index) {
    store.Set("key-" + std::to_string(key_index),
              "value-" + std::to_string(key_index));
  }

  std::atomic<bool> start{false};
  std::atomic<int> ready{0};
  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);

  for (int reader_index = 0; reader_index < kReaders; ++reader_index) {
    readers.emplace_back([&, reader_index] {
      ready.fetch_add(1, std::memory_order_release);
      WaitForStart(start);
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        const int key_index = (reader_index + iteration) % kKeys;
        const std::string key = "key-" + std::to_string(key_index);
        const std::string expected = "value-" + std::to_string(key_index);
        const std::optional<std::string> value = store.Get(key);
        if (!store.Contains(key) || !value.has_value() ||
            value.value() != expected || store.Size() != kKeys) {
          valid.store(false, std::memory_order_release);
        }
      }
    });
  }

  WaitForReady(ready, kReaders);
  start.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_TRUE(valid.load(std::memory_order_acquire));
}

TEST(KVStoreConcurrencyTest, ConcurrentWritesProduceCorrectFinalSize) {
  constexpr int kWriters = 6;
  constexpr int kKeysPerWriter = 150;

  KVStore store;
  std::atomic<bool> start{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);

  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    writers.emplace_back([&, writer_index] {
      ready.fetch_add(1, std::memory_order_release);
      WaitForStart(start);
      for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
        store.Set(KeyFor(writer_index, key_index),
                  ValueFor(writer_index, key_index));
      }
    });
  }

  WaitForReady(ready, kWriters);
  start.store(true, std::memory_order_release);
  for (std::thread& writer : writers) {
    writer.join();
  }

  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter),
            store.Size());
  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    EXPECT_EQ(ValueFor(writer_index, 0),
              store.Get(KeyFor(writer_index, 0)).value());
    EXPECT_EQ(ValueFor(writer_index, kKeysPerWriter / 2),
              store.Get(KeyFor(writer_index, kKeysPerWriter / 2)).value());
    EXPECT_EQ(ValueFor(writer_index, kKeysPerWriter - 1),
              store.Get(KeyFor(writer_index, kKeysPerWriter - 1)).value());
  }
}

TEST(KVStoreConcurrencyTest, ConcurrentReadersAndWritersRemainConsistent) {
  constexpr int kWriters = 4;
  constexpr int kReaders = 6;
  constexpr int kKeysPerWriter = 120;
  constexpr int kReaderIterations = 5000;

  KVStore store;
  for (int key_index = 0; key_index < 32; ++key_index) {
    store.Set("preload-" + std::to_string(key_index), "stable");
  }

  std::atomic<bool> start{false};
  std::atomic<int> ready{0};
  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    threads.emplace_back([&, writer_index] {
      ready.fetch_add(1, std::memory_order_release);
      WaitForStart(start);
      for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
        store.Set(KeyFor(writer_index, key_index),
                  ValueFor(writer_index, key_index));
      }
    });
  }

  for (int reader_index = 0; reader_index < kReaders; ++reader_index) {
    threads.emplace_back([&, reader_index] {
      ready.fetch_add(1, std::memory_order_release);
      WaitForStart(start);
      for (int iteration = 0; iteration < kReaderIterations; ++iteration) {
        const int writer_index = (reader_index + iteration) % kWriters;
        const int key_index = iteration % kKeysPerWriter;
        const std::optional<std::string> value =
            store.Get(KeyFor(writer_index, key_index));
        if (value.has_value() &&
            value.value() != ValueFor(writer_index, key_index)) {
          valid.store(false, std::memory_order_release);
        }
      }
    });
  }

  WaitForReady(ready, kWriters + kReaders);
  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(valid.load(std::memory_order_acquire));
  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter + 32),
            store.Size());
  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    VerifyThreadKeyRange(store, writer_index, kKeysPerWriter);
  }
}

TEST(KVStoreConcurrencyTest, ConcurrentDeletesAndWrites) {
  constexpr int kWriters = 4;
  constexpr int kKeysPerWriter = 100;

  KVStore store;
  std::atomic<bool> write_start{false};
  std::atomic<int> writers_ready{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);

  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    writers.emplace_back([&, writer_index] {
      writers_ready.fetch_add(1, std::memory_order_release);
      WaitForStart(write_start);
      for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
        store.Set(KeyFor(writer_index, key_index),
                  ValueFor(writer_index, key_index));
      }
    });
  }

  WaitForReady(writers_ready, kWriters);
  write_start.store(true, std::memory_order_release);
  for (std::thread& writer : writers) {
    writer.join();
  }

  std::atomic<bool> delete_start{false};
  std::atomic<int> deleters_ready{0};
  std::vector<std::thread> deleters;
  deleters.reserve(kWriters);

  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    deleters.emplace_back([&, writer_index] {
      deleters_ready.fetch_add(1, std::memory_order_release);
      WaitForStart(delete_start);
      for (int key_index = 0; key_index < kKeysPerWriter; key_index += 2) {
        store.Delete(KeyFor(writer_index, key_index));
      }
    });
  }

  WaitForReady(deleters_ready, kWriters);
  delete_start.store(true, std::memory_order_release);
  for (std::thread& deleter : deleters) {
    deleter.join();
  }

  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter / 2),
            store.Size());
  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
      const std::string key = KeyFor(writer_index, key_index);
      if ((key_index % 2) == 0) {
        EXPECT_FALSE(store.Contains(key)) << key;
      } else {
        ASSERT_TRUE(store.Get(key).has_value()) << key;
        EXPECT_EQ(ValueFor(writer_index, key_index), store.Get(key).value())
            << key;
      }
    }
  }
}

TEST(KVStoreConcurrencyTest, ConcurrentWalBackedWritesRecoverCorrectly) {
  constexpr int kWriters = 5;
  constexpr int kKeysPerWriter = 80;

  TempDir temp_dir;
  const std::string wal_path = temp_dir.FilePath("concurrent.wal");

  {
    WriteAheadLog wal(wal_path);
    KVStore store(&wal);
    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
      writers.emplace_back([&, writer_index] {
        ready.fetch_add(1, std::memory_order_release);
        WaitForStart(start);
        for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
          store.Set(KeyFor(writer_index, key_index),
                    ValueFor(writer_index, key_index));
        }
      });
    }

    WaitForReady(ready, kWriters);
    start.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
      writer.join();
    }
  }

  WriteAheadLog wal(wal_path);
  KVStore recovered;
  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter),
            recovered.ReplayFromWal(wal));
  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter),
            recovered.Size());
  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    VerifyThreadKeyRange(recovered, writer_index, kKeysPerWriter);
  }
}

TEST(KVStoreConcurrencyTest, SnapshotDuringConcurrentReadsIsConsistent) {
  constexpr int kKeys = 300;
  constexpr int kReaders = 6;
  constexpr int kReaderIterations = 12000;

  TempDir temp_dir;
  const std::string snapshot_path = temp_dir.FilePath("readers.snapshot");
  Snapshot snapshot(snapshot_path);
  KVStore store(nullptr, &snapshot);

  for (int key_index = 0; key_index < kKeys; ++key_index) {
    store.Set("key-" + std::to_string(key_index),
              "value-" + std::to_string(key_index));
  }

  std::atomic<bool> start{false};
  std::atomic<int> ready{0};
  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);

  for (int reader_index = 0; reader_index < kReaders; ++reader_index) {
    readers.emplace_back([&, reader_index] {
      ready.fetch_add(1, std::memory_order_release);
      WaitForStart(start);
      for (int iteration = 0; iteration < kReaderIterations; ++iteration) {
        const int key_index = (reader_index + iteration) % kKeys;
        const std::optional<std::string> value =
            store.Get("key-" + std::to_string(key_index));
        if (!value.has_value() ||
            value.value() != "value-" + std::to_string(key_index)) {
          valid.store(false, std::memory_order_release);
        }
      }
    });
  }

  WaitForReady(ready, kReaders);
  start.store(true, std::memory_order_release);
  EXPECT_TRUE(store.SaveSnapshot());
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_TRUE(valid.load(std::memory_order_acquire));
  KVStore recovered;
  const SnapshotLoadResult result = recovered.LoadSnapshot(snapshot);
  ASSERT_TRUE(result.loaded);
  EXPECT_EQ(static_cast<std::size_t>(kKeys), recovered.Size());
  for (int key_index = 0; key_index < kKeys; ++key_index) {
    EXPECT_EQ("value-" + std::to_string(key_index),
              recovered.Get("key-" + std::to_string(key_index)).value());
  }
}

TEST(KVStoreConcurrencyTest, CompactionAfterConcurrentWritesRecoversCorrectly) {
  constexpr int kWriters = 4;
  constexpr int kKeysPerWriter = 100;
  constexpr int kTailKeys = 12;

  TempDir temp_dir;
  const std::string wal_path = temp_dir.FilePath("compact.wal");
  const std::string snapshot_path = temp_dir.FilePath("compact.snapshot");

  {
    WriteAheadLog wal(wal_path);
    Snapshot snapshot(snapshot_path);
    KVStore store(&wal, &snapshot);

    std::atomic<bool> start{false};
    std::atomic<int> ready{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);

    for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
      writers.emplace_back([&, writer_index] {
        ready.fetch_add(1, std::memory_order_release);
        WaitForStart(start);
        for (int key_index = 0; key_index < kKeysPerWriter; ++key_index) {
          store.Set(KeyFor(writer_index, key_index),
                    ValueFor(writer_index, key_index));
        }
      });
    }

    WaitForReady(ready, kWriters);
    start.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
      writer.join();
    }

    ASSERT_TRUE(store.CompactPersistence());
    for (int key_index = 0; key_index < kTailKeys; ++key_index) {
      store.Set("tail-" + std::to_string(key_index),
                "tail-value-" + std::to_string(key_index));
    }
  }

  WriteAheadLog wal(wal_path);
  Snapshot snapshot(snapshot_path);
  KVStore recovered;
  const SnapshotLoadResult snapshot_result = recovered.LoadSnapshot(snapshot);
  ASSERT_TRUE(snapshot_result.loaded);
  recovered.ReplayFromWal(wal, snapshot_result.wal_offset);

  EXPECT_EQ(static_cast<std::size_t>(kWriters * kKeysPerWriter + kTailKeys),
            recovered.Size());
  for (int writer_index = 0; writer_index < kWriters; ++writer_index) {
    VerifyThreadKeyRange(recovered, writer_index, kKeysPerWriter);
  }
  for (int key_index = 0; key_index < kTailKeys; ++key_index) {
    EXPECT_EQ("tail-value-" + std::to_string(key_index),
              recovered.Get("tail-" + std::to_string(key_index)).value());
  }
}

}  // namespace
