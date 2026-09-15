# Benchmark History

This file records benchmark runs that are useful for comparison. The current
5900X scaling results, including raw repetitions, are in
[Benchmarks](Benchmarks.md). The older EC2 run below used another machine and
an earlier implementation, so its throughput is not directly comparable.

## 2026-09-17 — Physical-core and SMT scaling

| Field | Value |
| --- | --- |
| Final engine commit | `596a72eb9c4df7ae037851acf30af471f48369c3` |
| Coarse-lock source commit | `1f3ec22870fa1b9ea130659ca6df22552991a5e0` |
| Machine | `agency-bench`, AMD Ryzen 9 5900X, 12 physical / 24 logical CPUs |
| Build | GCC 13.3.0, Release, `-O3 -DNDEBUG` |
| Placement | Pinned 1, 2, 4, 8, 12 physical workers; separate 24-worker SMT rows |
| Repetitions | Three 500 ms runs per scaling point |
| Filesystem | Home-directory ext4 on NVMe for all WAL measurements |
| Validation | Release, ThreadSanitizer, and ASan/UBSan CTest each passed 90/90 |
| Artifacts | [Final raw data](benchmark_artifacts/final_20260917/raw.csv), [median summary](benchmark_artifacts/final_20260917/summary.csv), [plot curves](benchmark_artifacts/final_20260917/plot_curves.csv), [matched coarse baseline](benchmark_artifacts/coarse_gcc13_20260917/summary.csv), [perf](benchmark_artifacts/perf_20260917/) |

The final suite produced 537 raw runs and 179 median rows. Shard-isolated
in-memory SET reached 125.77M ops/sec at 12 physical cores (10.23× and 85.3%
efficiency), while a one-hot-shard write workload fell to 2.46M ops/sec.
Uniform GET reached 88.88M ops/sec (6.19×), and uniform 70R/30W reached
34.19M ops/sec (2.61×). Sync WAL stayed near 3.0k writes/sec. GroupCommit WAL
reached 25.69k writes/sec at 12 writers, using 10.93 writes per sync on
average, and 44.41k writes/sec with 24 SMT workers.

The compiler-matched coarse-lock 12-worker baseline delivered 2.17M
disjoint-key SET/s and 1.54M uniform 70R/30W ops/s; the sharded engine
delivered 37.66M and 34.19M respectively. At one worker, uniform 70R/30W
regressed by about 6% because sharding adds routing work. The original GCC 12
coarse run is retained separately under
[coarse_gcc12_20260917](benchmark_artifacts/coarse_gcc12_20260917/).

Perf counters and syscall traces identify shard-lock contention in hot and
mixed workloads and the fdatasync rate as the durable-write limit. The full
methodology, read/write tail latencies, GroupCommit delay tradeoff, checkpoint
comparison, and caveats are in [Benchmarks](Benchmarks.md).

## 2026-05-25T08:35:47Z - First EC2 Baseline

| Field | Value |
| --- | --- |
| Date | `2026-05-25T08:35:47Z` |
| Commit recorded by metadata | `c9fb546a6f9d720d5184bc657fcbdd65096bd16b` |
| Published commit containing benchmark workflow changes | `d7adcb4` |
| Branch | `main` |
| EC2 public IPv4 | `3.20.238.237` |
| Instance type | AWS EC2 `c7i-flex.large` |
| Hostname | `ip-172-31-44-213` |
| CPU | Intel Xeon Platinum 8488C |
| vCPU | 2 |
| Memory | 3.7 GiB |
| OS | Ubuntu 26.04 LTS, Linux `7.0.0-1004-aws` |
| Compiler | GCC/G++ 15.2.0 |
| CMake | 4.2.3 |
| Build flags | `-DCMAKE_BUILD_TYPE=Release`; EC2 cache recorded `-O3 -DNDEBUG` |
| Benchmark script | `scripts/run_ec2_benchmarks.sh` |
| Raw result path | `/home/ubuntu/concurrent-kv-store/benchmark_results/20260525_083522/` |
| Google Benchmark settings | 5 repetitions, aggregate rows only, JSON output |
| CTest before benchmark | Not recorded by the benchmark script |

Summary:

| Benchmark | Input Size | Mean Time | Throughput |
| --- | ---: | ---: | ---: |
| Mixed 70/30 read-write | 1,000-key working set | `18.2 ns/op` | `55.09M ops/sec` |
| Get | 1,000-key preload | `20.8 ns/op` | `48.05M ops/sec` |
| Set | 1,000 writes/batch | `58.5 us/batch` | `17.11M ops/sec` |
| Delete | 1,000 deletes/batch | `33.7 us/batch` | `29.66M ops/sec` |
| Durable Set with WAL flush | 1,000 writes/batch | `603.2 us/batch` | `1.66M ops/sec` |
| WAL replay | 10,000 records | `3.75 ms` | `2.67M records/sec` |
| Snapshot load | 10,000 entries | `1.21 ms` | `8.28M entries/sec` |
| Snapshot + WAL-tail recovery | 10,000 base entries + 10% tail | `1.72 ms` | `6.38M entries/sec` |
| Snapshot compaction | 10,000 entries | `2.46 ms` | `4.07M entries/sec` |

Improvements/regressions:

- First official EC2 KV-store baseline; no prior curated EC2 KV benchmark
  history exists for apples-to-apples comparison.

Caveats:

- The run was not CPU-pinned.
- The benchmark script did not run or record CTest before executing benchmarks.
- Metadata recorded a dirty working tree. The benchmark workflow/source changes
  were committed afterward as `d7adcb4`; rerun from a clean commit if strict
  release provenance is required.
- Filesystem-sensitive rows may vary with EC2 storage/cache state.
- This run predates the Phase 0.5 `std::shared_mutex` lock insertion and CLI
  status updates.

Next steps:

- Add CTest execution and pass-count capture to the EC2 runner.
- Rerun from a clean commit for a stricter publication baseline.
- Add CLI/public-boundary benchmarks for parser/server/output formatting cost.
- Add read-only and mixed read/write contention rows for the coarse
  reader/writer-lock implementation.

## Historical EC2 entry template

Copy this block when publishing a new EC2 benchmark run:

```text
Date:
Commit:
Branch:
EC2 public IPv4: 3.20.238.237
Instance type:
CPU:
vCPU:
Memory:
OS:
Compiler:
CMake:
Build flags:
Benchmark script:
Raw result path:
CTest before benchmark:
Summary:
Improvements:
Regressions:
Caveats:
Concurrency model:
CLI status surface:
Next steps:
```

## Historical EC2 publication checklist

1. Run correctness tests before the benchmark, then record the pass count.
2. Run the EC2 workflow from the repository root:

   ```bash
   ssh ubuntu@3.20.238.237
   cd ~/concurrent-kv-store
   git pull
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ctest --test-dir build --output-on-failure -C Release
   chmod +x scripts/run_ec2_benchmarks.sh
   ./scripts/run_ec2_benchmarks.sh
   ```

3. Copy environment metadata from
   `benchmark_results/<YYYYMMDD_HHMMSS>/metadata.txt` into
   `docs/Benchmarks.md`.
4. Summarize aggregate Google Benchmark rows from
   `benchmark_results/<YYYYMMDD_HHMMSS>/benchmarks.json`.
5. Add one dated entry here with the commit, EC2 instance type, build flags,
   high-level summary, notable improvements/regressions, and raw artifact path.
6. Commit only curated docs updates unless a raw artifact is intentionally
   archived elsewhere.
