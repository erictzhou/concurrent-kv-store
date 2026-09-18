# Concurrent KV Store

A small C++20 key-value engine with 64 in-memory shards, checksum-framed WAL
recovery, explicit write acknowledgement policies, and snapshot checkpoints.
The storage API is thread-safe; the CLI is a single-process interactive front
end.

## What changed in v0.6.0

The previous store put every operation through one `std::shared_mutex`.
Measurements on a 12-core Ryzen 9 5900X showed that disjoint writes lost
throughput as writers were added. Foreground `Get`, `Set`, `Contains`, and
`Delete` now lock only the key's shard. Global operations acquire all shard
locks in a fixed order. WAL writes use an ordered writer for GroupCommit, and
automatic snapshots write in the background after a consistent in-memory
capture.

The pinned multicore results, raw CSV files, and profiling evidence are in
[Benchmarks](docs/Benchmarks.md). The older EC2 microbenchmark tables remain
in [Benchmark History](docs/Benchmark_History.md) and describe a different
machine and earlier implementation.

On the 12-core 5900X, shard-isolated in-memory writes reach 125.52M ops/sec
(10.29× one core). Uniform reads reach 88.98M ops/sec (6.19×). Sync WAL
stays near 2.96k writes/sec; GroupCommit reaches 25.68k writes/sec by sharing
one sync across an average of 10.97 writes. One-hot-shard writes remain
serialized and do not scale.

## Write acknowledgement

| Mode | When `Set` or `Delete` returns | Crash guarantee |
| --- | --- | --- |
| In-memory | The shard map has changed; no WAL is attached. | None. |
| Buffered WAL | The framed record has been flushed from the C++ stream into the kernel page cache, then memory changes. | A process crash can usually replay it; a power loss can lose it. |
| Sync WAL | The record is flushed and `fdatasync()` succeeds, then memory changes. | The acknowledged record is synchronized to the filesystem. |
| GroupCommit WAL | The ordered writer writes a batch and `fdatasync()` succeeds for that batch, then each caller changes memory and returns. | The acknowledged batch is synchronized to the filesystem. |

The default CLI uses Buffered WAL. It does not claim stable-storage durability.
The `WriteAheadLog` constructor accepts `DurabilityPolicy::Sync` or
`DurabilityPolicy::GroupCommit` for callers that require a synchronization
boundary. GroupCommit defaults to 32 records or a 20 µs batching window.
Errors propagate to waiting writers; a failed write is not applied to memory.

New WAL files use v3: a checksummed, versioned header records byte order and
generation, and little-endian frames carry CRC32 and ordered sequence
numbers. Recovery applies only complete validated records and can truncate a
bad tail. Existing v2 files remain readable and are upgraded on rotation.
Snapshots still use a native-endian format; see
[WAL Format](docs/WAL_Format.md) and [Architecture](docs/Architecture.md)
for ordering and remaining limits.

## Build and use

Prerequisites: CMake 3.20+, a C++20 compiler, and network access on first
configure for GoogleTest and Google Benchmark.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/kv_store
```

CLI commands: `SET`, `GET`, `DEL`/`DELETE`, `COMPACT`/`SNAPSHOT`,
`CLEAR PERSISTENCE`, `INFO`/`VERSION`/`STATUS`, `HELP`, and `EXIT`.

Automatic checkpoint requests start after 1,000 writes. A background thread
captures the map and current WAL offset, then writes and synchronizes the
snapshot while normal key operations continue. Automatic checkpoints retain
the WAL. `COMPACT` writes a verified snapshot and rotates the WAL explicitly.
`WaitForCheckpoints()` waits for scheduled work and reports background errors.

## Benchmarks

The multicore runner is Linux-only because it pins every application worker to
a chosen CPU. It detects physical cores and schedules `1, 2, 4, 8, …, N`
workers on distinct cores. SMT is a separate 24-thread comparison on
`agency-bench`.

```bash
cmake --build build --target kv_store_scaling_benchmark
python3 scripts/run_scaling_benchmarks.py \
  --binary build/kv_store_scaling_benchmark \
  --output benchmark_results/local_scaling --scope final \
  --duration-ms 500 --repetitions 3
```

The runner records raw repetitions and median CSV rows with throughput,
speedup, efficiency, separate read/write latency percentiles, worker CPU IDs,
physical-core count, and WAL batch/sync metrics. `scripts/run_group_delay_sweep.py`
tests four batching windows. The existing Google Benchmark hot-path suite is
still available as `kv_store_benchmark`; its earlier WAL-flush rows should be
read as Buffered, not synchronized, writes.

## Repository map

| Path | Purpose |
| --- | --- |
| `include/`, `src/` | Store, WAL, snapshot, parser, and CLI |
| `tests/` | Unit, integration, concurrency, recovery, and stress tests |
| `benchmarks/scaling/` | Pinned throughput, latency, and checkpoint benchmarks |
| `benchmarks/core_hot_path/` | Earlier single-thread Google Benchmark suite |
| `docs/` | Architecture, benchmark results, history, changelog, and roadmap |

The engine does not implement SSTables, an LSM tree, network access,
replication, or distributed coordination.
