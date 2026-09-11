#include "persistence/wal.h"
#include "store/kv_store.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kKeysPerWorker = 10000;
constexpr std::size_t kSharedKeys = 10000;
constexpr std::size_t kTraceLength = 65536;
constexpr std::size_t kShardCount = 64;

struct Options {
  std::string mode = "memory";
  std::string distribution = "uniform";
  int read_percent = 100;
  int duration_ms = 1000;
  int threads = 1;
  double zipf_s = 0.99;
  int group_delay_us = 200;
  int group_max_batch = 32;
  std::vector<int> cpus{0};
  std::string wal_path;
};

std::vector<int> ParseCpus(const std::string& text) {
  std::vector<int> cpus;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t end = text.find(',', begin);
    cpus.push_back(std::stoi(text.substr(begin, end - begin)));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return cpus;
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    if (i + 1 >= argc) throw std::runtime_error("missing option value");
    const std::string name = argv[i];
    const std::string value = argv[++i];
    if (name == "--mode") options.mode = value;
    else if (name == "--distribution") options.distribution = value;
    else if (name == "--read-percent") options.read_percent = std::stoi(value);
    else if (name == "--duration-ms") options.duration_ms = std::stoi(value);
    else if (name == "--threads") options.threads = std::stoi(value);
    else if (name == "--cpus") options.cpus = ParseCpus(value);
    else if (name == "--zipf-s") options.zipf_s = std::stod(value);
    else if (name == "--group-delay-us") options.group_delay_us = std::stoi(value);
    else if (name == "--group-max-batch") options.group_max_batch = std::stoi(value);
    else if (name == "--wal-path") options.wal_path = value;
    else throw std::runtime_error("unknown option: " + name);
  }
  if (options.threads < 1 || options.duration_ms < 1 ||
      options.read_percent < 0 || options.read_percent > 100 ||
      options.cpus.size() != static_cast<std::size_t>(options.threads) ||
      options.zipf_s <= 0 || options.group_delay_us < 0 ||
      options.group_max_batch < 1) {
    throw std::runtime_error("invalid threads, CPUs, duration, mix, or Zipf s");
  }
  if (options.mode != "memory" && options.mode != "buffered" &&
      options.mode != "sync" && options.mode != "group") {
    throw std::runtime_error("invalid storage mode");
  }
  if (options.distribution != "uniform" && options.distribution != "zipf" &&
      options.distribution != "disjoint" && options.distribution != "disjoint-shard" &&
      options.distribution != "hot-shard" &&
      options.distribution != "single-key") {
    throw std::runtime_error("invalid key distribution");
  }
  if (options.mode != "memory" && options.wal_path.empty()) {
    throw std::runtime_error("--wal-path is required for WAL modes");
  }
  return options;
}

void PinToCpu(int cpu) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    throw std::runtime_error("worker affinity failed for CPU " + std::to_string(cpu));
  }
#else
  (void)cpu;
  throw std::runtime_error("CPU pinning requires Linux");
#endif
}

struct Op {
  std::uint32_t key_index;
  bool write;
};

std::vector<Op> MakeTrace(const Options& options, int worker, std::size_t key_count) {
  std::mt19937_64 rng(0x5eed1234ULL + static_cast<std::uint64_t>(worker));
  std::uniform_int_distribution<std::uint32_t> uniform(0, static_cast<std::uint32_t>(key_count - 1));
  std::uniform_int_distribution<int> mix(0, 99);
  std::vector<double> weights;
  if (options.distribution == "zipf") {
    weights.reserve(key_count);
    for (std::size_t i = 0; i < key_count; ++i) {
      weights.push_back(1.0 / std::pow(static_cast<double>(i + 1), options.zipf_s));
    }
  }
  std::discrete_distribution<std::uint32_t> zipf(weights.begin(), weights.end());
  std::vector<Op> trace;
  trace.reserve(kTraceLength);
  for (std::size_t i = 0; i < kTraceLength; ++i) {
    const auto index = options.distribution == "single-key" ? 0U :
                       options.distribution == "zipf" ? zipf(rng) : uniform(rng);
    trace.push_back({index, mix(rng) >= options.read_percent});
  }
  return trace;
}

std::vector<std::string> MakeKeys(const Options& options, int worker) {
  const std::size_t count = options.distribution == "disjoint" ||
                            options.distribution == "disjoint-shard" ? kKeysPerWorker : kSharedKeys;
  std::vector<std::string> keys;
  keys.reserve(count);
  if (options.distribution == "hot-shard") {
    for (std::size_t candidate = 0; keys.size() < count; ++candidate) {
      std::string key = "key-" + std::to_string(candidate);
      if ((std::hash<std::string>{}(key) & (kShardCount - 1)) == 0) {
        keys.push_back(std::move(key));
      }
    }
  } else if (options.distribution == "disjoint-shard") {
    for (std::size_t candidate = 0; keys.size() < count; ++candidate) {
      std::string key = "shard-key-" + std::to_string(worker) + "-" +
                        std::to_string(candidate);
      const auto shard = std::hash<std::string>{}(key) & (kShardCount - 1);
      if (shard % static_cast<std::size_t>(options.threads) ==
          static_cast<std::size_t>(worker)) {
        keys.push_back(std::move(key));
      }
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t id = options.distribution == "disjoint"
                                 ? static_cast<std::size_t>(worker) * count + i : i;
      keys.push_back("key-" + std::to_string(id));
    }
  }
  return keys;
}

struct Stats {
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  std::vector<std::uint64_t> read_ns;
  std::vector<std::uint64_t> write_ns;
};

std::uint64_t Quantile(std::vector<std::uint64_t>& values, double q) {
  if (values.empty()) return 0;
  const std::size_t index = static_cast<std::size_t>(
      std::ceil(q * static_cast<double>(values.size()))) - 1;
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

void PrintLatencies(const std::string& prefix, const std::vector<std::uint64_t>& source) {
  const std::uint64_t sample_count = source.size();
  std::cout << ',' << prefix << "_samples=" << sample_count;
  for (const auto [name, q] : {std::pair{"p50", 0.5}, {"p95", 0.95},
                                {"p99", 0.99}, {"p999", 0.999}}) {
    auto values = source;
    std::cout << ',' << prefix << '_' << name << "_ns=" << Quantile(values, q);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseOptions(argc, argv);
    std::unique_ptr<kv::persistence::WriteAheadLog> wal;
    std::unique_ptr<kv::store::KVStore> store;
    std::vector<std::vector<std::string>> keys;
    std::vector<std::vector<Op>> traces;
    keys.reserve(options.threads);
    traces.reserve(options.threads);
    for (int worker = 0; worker < options.threads; ++worker) {
      keys.push_back(MakeKeys(options, worker));
      traces.push_back(MakeTrace(options, worker, keys.back().size()));
    }

    auto preload = [&](kv::store::KVStore& target) {
      for (int worker = 0; worker < options.threads; ++worker) {
        if (worker == 0 || options.distribution == "disjoint" ||
            options.distribution == "disjoint-shard") {
          for (const auto& key : keys[worker]) {
            target.Set(key, "value-0000000000000000");
          }
        }
      }
    };
    if (options.mode == "memory") {
      store = std::make_unique<kv::store::KVStore>();
      preload(*store);
    } else {
      // Build the common initial state with buffered WAL, then recover it into
      // the chosen policy. Preload syncs do not contaminate the timed interval.
      {
        kv::persistence::WriteAheadLog preload_wal(options.wal_path);
        preload_wal.Clear();
        kv::store::KVStore preload_store(&preload_wal);
        preload(preload_store);
      }
      const auto policy = options.mode == "sync" ? kv::persistence::DurabilityPolicy::Sync :
                          options.mode == "group" ? kv::persistence::DurabilityPolicy::GroupCommit :
                                                    kv::persistence::DurabilityPolicy::Buffered;
      wal = std::make_unique<kv::persistence::WriteAheadLog>(
          options.wal_path, policy, static_cast<std::size_t>(options.group_max_batch),
          static_cast<std::uint32_t>(options.group_delay_us));
      if (options.mode == "group") wal->PinWriterToCpu(options.cpus.front());
      store = std::make_unique<kv::store::KVStore>(wal.get());
      store->ReplayFromWal(*wal);
      wal->Clear();
      wal->ResetStats();
    }

    std::vector<Stats> stats(options.threads);
    std::atomic<bool> stop{false};
    std::barrier ready(options.threads + 1);
    std::vector<std::thread> workers;
    workers.reserve(options.threads);
    std::exception_ptr worker_error;
    std::mutex error_mutex;
    for (int worker = 0; worker < options.threads; ++worker) {
      workers.emplace_back([&, worker] {
        try {
          PinToCpu(options.cpus[worker]);
        } catch (...) {
          std::lock_guard lock(error_mutex);
          if (!worker_error) worker_error = std::current_exception();
          stop.store(true, std::memory_order_relaxed);
        }
        ready.arrive_and_wait();
        try {
          if (stop.load(std::memory_order_relaxed)) return;
          const auto& worker_keys = keys[worker];
          const auto& trace = traces[worker];
          Stats& result = stats[worker];
          std::size_t index = 0;
          const std::size_t sample_mask = options.mode == "memory" ? 31 :
                                          options.mode == "buffered" ? 7 : 0;
          while (!stop.load(std::memory_order_relaxed)) {
            const Op& op = trace[index & (kTraceLength - 1)];
            const bool sample = (index & sample_mask) == 0;
            const auto start = sample ? Clock::now() : Clock::time_point{};
            if (op.write) {
              store->Set(worker_keys[op.key_index], "value-1111111111111111");
              ++result.writes;
            } else {
              const auto value = store->Get(worker_keys[op.key_index]);
              if (!value) throw std::runtime_error("missing preloaded key");
              ++result.reads;
            }
            if (sample) {
              const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  Clock::now() - start).count();
              (op.write ? result.write_ns : result.read_ns).push_back(
                  static_cast<std::uint64_t>(ns));
            }
            ++index;
          }
        } catch (...) {
          std::lock_guard lock(error_mutex);
          if (!worker_error) worker_error = std::current_exception();
          stop.store(true, std::memory_order_relaxed);
        }
      });
    }
    const auto start = Clock::now();
    ready.arrive_and_wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(options.duration_ms));
    stop.store(true, std::memory_order_relaxed);
    for (auto& worker : workers) worker.join();
    if (worker_error) std::rethrow_exception(worker_error);
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::uint64_t reads = 0, writes = 0;
    std::vector<std::uint64_t> read_ns, write_ns;
    for (auto& result : stats) {
      reads += result.reads;
      writes += result.writes;
      read_ns.insert(read_ns.end(), result.read_ns.begin(), result.read_ns.end());
      write_ns.insert(write_ns.end(), result.write_ns.begin(), result.write_ns.end());
    }
    std::cout << "mode=" << options.mode << ",distribution=" << options.distribution
              << ",read_percent=" << options.read_percent << ",threads=" << options.threads
              << ",group_delay_us=" << (options.mode == "group" ? options.group_delay_us : 0)
              << ",group_max_batch=" << (options.mode == "group" ? options.group_max_batch : 0)
              << ",seconds=" << seconds << ",reads=" << reads << ",writes=" << writes
              << ",ops_per_sec=" << (reads + writes) / seconds;
    PrintLatencies("read", read_ns);
    PrintLatencies("write", write_ns);
    if (wal) {
      auto wal_stats = wal->GetStats();
      const double average_batch = wal_stats.sync_calls == 0 ? 0.0 :
          static_cast<double>(wal_stats.records) / wal_stats.sync_calls;
      auto batch_quantile = [&](double q) {
        if (wal_stats.sync_calls == 0) return std::size_t{0};
        const auto target = static_cast<std::uint64_t>(
            std::ceil(q * static_cast<double>(wal_stats.sync_calls)));
        std::uint64_t cumulative = 0;
        for (std::size_t batch = 0; batch < wal_stats.batch_histogram.size(); ++batch) {
          cumulative += wal_stats.batch_histogram[batch];
          if (cumulative >= target) return batch;
        }
        return std::size_t{0};
      };
      std::cout << ",average_batch_size=" << average_batch
                << ",batch_p95=" << batch_quantile(0.95)
                << ",batch_p99=" << batch_quantile(0.99)
                << ",fdatasync_calls_per_sec=" << wal_stats.sync_calls / seconds
                << ",writes_per_sync=" << (wal_stats.sync_calls == 0 ? 0.0 :
                      static_cast<double>(wal_stats.records) / wal_stats.sync_calls);
    }
    std::cout << '\n';
  } catch (const std::exception& e) {
    std::cerr << "benchmark error: " << e.what() << '\n';
    return 1;
  }
}
