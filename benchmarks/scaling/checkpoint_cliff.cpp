#include "persistence/snapshot.h"
#include "store/kv_store.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

std::uint64_t Quantile(std::vector<std::uint64_t> values, double quantile) {
  const auto index = static_cast<std::size_t>(quantile * (values.size() - 1));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

void Pin(int cpu) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    throw std::runtime_error("checkpoint writer affinity failed");
  }
#else
  (void)cpu;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: checkpoint_cliff SNAPSHOT_PATH CPU\n";
    return 2;
  }
  try {
    Pin(std::stoi(argv[2]));
    kv::persistence::Snapshot snapshot(argv[1]);
    kv::store::KVStore store(nullptr, &snapshot);
    std::vector<std::string> keys;
    for (int i = 0; i < 1000; ++i) keys.push_back("key-" + std::to_string(i));
    for (const auto& key : keys) store.Set(key, "value");
    constexpr std::size_t kOperations = 20000;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(kOperations);
    const auto wall_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kOperations; ++i) {
      const auto start = std::chrono::steady_clock::now();
      store.Set(keys[i % keys.size()], "new-value");
      latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count());
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wall_start).count();
    std::cout << "ops_per_sec=" << kOperations / seconds
              << ",p50_ns=" << Quantile(latencies, 0.5)
              << ",p95_ns=" << Quantile(latencies, 0.95)
              << ",p99_ns=" << Quantile(latencies, 0.99)
              << ",p999_ns=" << Quantile(latencies, 0.999)
              << ",max_ns=" << *std::max_element(latencies.begin(), latencies.end())
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
