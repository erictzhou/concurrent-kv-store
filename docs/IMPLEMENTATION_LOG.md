# Implementation log

## 2026-09-17

- Inspected the repository at `1f3ec22`. It had one store-wide
  `std::shared_mutex`, an `ofstream` WAL with no `fdatasync`, and no concurrent
  benchmark rows. The earlier published EC2 numbers were from another host.
- Inspected `agency-bench`: Ryzen 9 5900X, 12 physical cores, 24 logical CPUs,
  two L3 cache domains, one NUMA node. Physical worker order is
  `0,6,1,7,2,8,3,9,4,10,5,11`; SMT siblings are separate. The governor was
  `powersave`, boost enabled. Host lacked GCC/CMake, so the Release build and
  CTest ran in the repository Docker image and its binary was copied to the
  host for pinned measurements.
- Captured a three-repetition, 500 ms coarse-lock baseline before changing
  store internals. Raw and median CSV files are under
  `docs/benchmark_artifacts/coarse_20260917/`. The first runner completed all
  raw rows but hit a string/integer mismatch while computing summary speedup;
  summary was regenerated from those raw rows without rerunning measurements.
- Added 64 shards and explicit Buffered, Sync, and GroupCommit policies.
  GroupCommit serializes a batch in one writer thread, calls `fdatasync` once,
  and wakes all requests after the sync. A bounded batch histogram records
  batching without unbounded metrics memory.
- Found `/tmp` is `tmpfs` on `agency-bench`; an early sync smoke test there
  measured memory-backed synchronization. All publication WAL files were
  moved to the home directory's ext4 volume. A 2-thread ext4 Sync smoke test
  reached about 3.0k writes/sec; a 4-thread GroupCommit smoke test reached
  about 6.5k writes/sec at roughly 3.9 writes/sync.
- The first sharded sweep still reached only 1.32× at 12 threads for disjoint
  writes. The hot path was taking a shared administrative lock per operation.
  Removed that global lock from per-key operations; global operations now lock
  every shard in a fixed order. A 12-thread shard-isolated smoke test then
  reached about 128M writes/sec.
- Added background automatic checkpoints: foreground writers schedule bounded
  work, a worker captures the map and WAL offset, and snapshot I/O runs after
  releasing shard locks. Fixed a scheduling race with persistence reset by
  posting the request before releasing the triggering shard lock.
- Synchronized snapshot temporary files before rename, synchronized the
  parent directory after publication, and synchronized WAL truncation in Sync
  and GroupCommit modes. Kept automatic WAL history rather than discarding it
  under active writes.
- Clean Release CTest on `agency-bench` passed 89/89 for commit `34c7420`.
  The pinned publication sweep for that commit was then launched. Perf and
  final benchmark findings follow after measurements complete.
- The first complete sharded sweep at 200 µs GroupCommit delay had 537 raw
  runs. At 12 physical writers, shard-isolated in-memory writes reached
  125.1M ops/sec (10.3×), while Sync writes held near 3.0k ops/sec and
  GroupCommit reached 17.9k ops/sec at about 10.9 writes/sync. The full raw
  data is preserved as `pre_tuning_20260917`.
- Swept GroupCommit windows 0, 20, 200, and 1,000 µs on ext4. At 12 writers,
  20 µs reached 26.1k writes/sec with about 1.19 ms p99, versus 17.9k and
  1.41 ms at 200 µs. Changed the default to 20 µs and retained raw delay
  artifacts. Added a high-contention GroupCommit recovery stress test; local
  Release CTest passed 90/90 before the tuned publication rerun.
- Bounded total GroupCommit batch bytes to one maximum-sized WAL frame. Before
  this fix, 4,096 individually valid large requests could cause an oversized
  aggregate allocation. Local Release CTest still passed 90/90.
- Found `Size()` acquired all shard shared locks and then reacquired each
  lock while counting. Removed the redundant second acquisition. Rebuilt and
  retested the corrected commit `596a72e`: local and remote Release CTest
  both passed 90/90.
- The user required a persistent remote tmux session. Checked for
  `kvstore`, found none, created it, and used windows within that session
  for every subsequent remote build, benchmark, perf run, checkpoint
  comparison, and sanitizer run. An in-progress pre-tmux sweep was stopped
  and preserved as partial data. The `kvstore` session remains running.
- On `agency-bench`, built the exact `596a72e` source in the Ubuntu 24.04
  Docker image with GCC 13.3.0. The pinned final runner executed 537 raw
  runs (three 500 ms repetitions at each point), including 1/2/4/8/12
  physical-core sweeps and separate 24-thread SMT comparisons. Exported
  179 summary rows and 28 plot-ready curve rows. Preserved the binary hash
  and topology metadata with the artifacts.
- Ran `scripts/profile_scaling.py` under tmux for 1, 6, and 12 workers:
  `perf stat` captured cycles, instructions, cache references/misses,
  branches, switches, and migrations; `strace -f -c` captured futex,
  write, and fdatasync calls for Sync and GroupCommit. A separate
  `perf record` confirmed read-lock cost and hot-shard scheduling activity.
  Dedicated LLC-load counters were unavailable, so only generic cache
  counters are reported.
- Rebuilt the preserved coarse-lock source with the same GCC 13.3 toolchain
  and benchmark harness, then collected 315 raw baseline runs. The earlier
  GCC 12 baseline is retained separately and excluded from the matched
  before/after comparison.
- The corrected final run measured 125.77M shard-isolated SET/s at 12 cores
  (10.23×), 88.88M uniform GET/s (6.19×), 34.19M uniform 70R/30W ops/s
  (2.61×), 2.95k Sync SET/s, and 25.69k GroupCommit SET/s. Hot-shard SET
  fell to 2.46M/s, with 491k context switches during its 12-worker perf run.
  GroupCommit averaged 10.93 writes/sync at 12 writers; Sync stayed near
  one write/sync and its uniform-write p99 grew to 110 ms.
- The checkpoint comparison used five repetitions of 20,000 SETs over 1,000
  preloaded keys. Median p99.9 dropped from 7,000 ns for the old foreground
  snapshot design to 110 ns with background publication, although rare
  maximum pauses remained around 0.2–0.3 ms.
- Remote ASan/UBSan CTest passed 90/90. GCC ThreadSanitizer initially failed
  at startup with an unexpected memory mapping, then its lock-order tracker
  exceeded 64 simultaneously held shard locks. Running the container with
  ASLR disabled and `TSAN_OPTIONS=detect_deadlocks=0:halt_on_error=1`
  allowed data-race instrumentation to run; the full TSan CTest passed 90/90.
  Local AppleClang TSan also failed to start a trivial standalone program, so
  the successful remote run is the sanitizer evidence.
