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
