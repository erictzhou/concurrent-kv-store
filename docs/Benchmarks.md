# Multicore benchmarks — Ryzen 9 5900X

The 64-shard engine scales well when writers own different shards: 125.77M in-memory SET/s at 12 physical cores, 10.23× its one-core rate and 85.3% parallel efficiency. Shared uniform writes reach 31.40M/s (2.20×); one hot shard falls to 2.46M/s (0.18×). The single WAL file is a different limit: Sync stays near 3.0k writes/s, while GroupCommit reaches 25.69k writes/s by putting an average of 10.93 writes in each sync.

## Machine, provenance, and method

| Item | Recorded value |
| --- | --- |
| Host | agency-bench, AMD Ryzen 9 5900X, Linux 7.0.0-31-generic |
| Topology | N = 12 physical cores, 24 logical CPUs, one NUMA node, two 32 MiB L3 domains |
| Physical worker CPU order | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5, 11 |
| SMT sibling order | 12, 18, 13, 19, 14, 20, 15, 21, 16, 22, 17, 23 |
| CPU state | powersave governor, boost enabled; no fixed-frequency isolation |
| WAL output | Home-directory ext4 on /dev/nvme1n1p2; /tmp is tmpfs and was excluded |
| Build | Ubuntu 24.04 container, GCC 13.3.0, CMake 3.28.3, Release (-O3 -DNDEBUG) |
| Final engine source | 596a72eb9c4df7ae037851acf30af471f48369c3 |
| Scaling binary SHA-256 | 071233a0709fbc230af9fbc30cb71106377fc536fcbe5078c9df680101a77a65 |
| Pre-run validation | Remote Release CTest: 90/90 passed |
| Repetitions | Three adjacent 500 ms runs per point; tables use the median |
| Physical sweep | 1, 2, 4, 8, and 12 worker threads, one worker per distinct physical core |
| SMT comparison | 24 workers on the same 12 cores, reported separately |
| Workloads | 10,000 shared keys or 10,000 thread-owned keys per worker; Zipf exponent s = 0.99 |
| Latency sampling | Every 32nd memory op, every 8th Buffered op, every Sync/GroupCommit op |
| Tail rule | p99.9 omitted when the median sample count is below 10,000 |

Application workers are pinned with pthread affinity. The GroupCommit writer is pinned to CPU 0, the first worker CPU. Thus its extra thread shares that CPU in the physical sweep. The CPU lists and physical-core count appear in every summary row. Key generation, preloading, and trace generation happen before the timed interval. The timed workload is all GET, all SET, or the stated read/write mix. The read and write latency columns are separate. Throughput is aggregate completed operations divided by elapsed wall time.

Speedup(T) = throughput(T) / throughput(1), and parallel efficiency(T) = speedup(T) / T. The full [raw repetitions](benchmark_artifacts/final_20260917/raw.csv), [179-row median summary](benchmark_artifacts/final_20260917/summary.csv), [28-row plot-ready curves](benchmark_artifacts/final_20260917/plot_curves.csv), and [machine metadata](benchmark_artifacts/final_20260917/metadata.json) provide throughput, speedup, efficiency, physical cores, p50/p95/p99/p99.9, and variation for every measured combination. The plot file contains read-only, shard-isolated in-memory writes, mixed 70R/30W, Sync writes, and GroupCommit writes, with SMT rows marked separately.

## Physical-core scaling curves

Each p99 is for the operation named in the workload, except the mixed curve, which shows read/write p99 separately. These five curves are also in the plot-ready CSV.

| Workload | Workers / cores | Ops/s | Speedup | Efficiency | p99 latency |
| --- | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 1 / 1 | 14.36M | 1.00× | 100.0% | read 0.12 µs |
| GET uniform | 2 / 2 | 17.95M | 1.25× | 62.5% | read 0.23 µs |
| GET uniform | 4 / 4 | 33.17M | 2.31× | 57.7% | read 0.29 µs |
| GET uniform | 8 / 8 | 61.22M | 4.26× | 53.3% | read 0.36 µs |
| GET uniform | 12 / 12 | 88.88M | 6.19× | 51.6% | read 0.37 µs |
| SET shard-isolated, memory | 1 / 1 | 12.29M | 1.00× | 100.0% | write 0.14 µs |
| SET shard-isolated, memory | 2 / 2 | 23.21M | 1.89× | 94.4% | write 0.18 µs |
| SET shard-isolated, memory | 4 / 4 | 45.05M | 3.66× | 91.6% | write 0.20 µs |
| SET shard-isolated, memory | 8 / 8 | 87.28M | 7.10× | 88.7% | write 0.22 µs |
| SET shard-isolated, memory | 12 / 12 | 125.77M | 10.23× | 85.3% | write 0.23 µs |
| 70R/30W uniform, memory | 1 / 1 | 13.10M | 1.00× | 100.0% | read 0.13 / write 0.14 µs |
| 70R/30W uniform, memory | 2 / 2 | 12.35M | 0.94× | 47.1% | read 0.38 / write 0.96 µs |
| 70R/30W uniform, memory | 4 / 4 | 20.48M | 1.56× | 39.1% | read 0.81 / write 1.28 µs |
| 70R/30W uniform, memory | 8 / 8 | 29.84M | 2.28× | 28.5% | read 0.98 / write 6.52 µs |
| 70R/30W uniform, memory | 12 / 12 | 34.19M | 2.61× | 21.7% | read 3.29 / write 9.50 µs |
| SET Sync WAL | 1 / 1 | 3.07k | 1.00× | 100.0% | write 0.372 ms |
| SET Sync WAL | 2 / 2 | 2.99k | 0.97× | 48.7% | write 10.99 ms |
| SET Sync WAL | 4 / 4 | 2.96k | 0.96× | 24.1% | write 41.96 ms |
| SET Sync WAL | 8 / 8 | 2.95k | 0.96× | 12.0% | write 85.16 ms |
| SET Sync WAL | 12 / 12 | 2.95k | 0.96× | 8.0% | write 110.22 ms |
| SET GroupCommit WAL | 1 / 1 | 2.45k | 1.00× | 100.0% | write 0.450 ms |
| SET GroupCommit WAL | 2 / 2 | 4.81k | 1.96× | 98.2% | write 0.651 ms |
| SET GroupCommit WAL | 4 / 4 | 9.40k | 3.84× | 95.9% | write 0.822 ms |
| SET GroupCommit WAL | 8 / 8 | 17.98k | 7.34× | 91.8% | write 0.891 ms |
| SET GroupCommit WAL | 12 / 12 | 25.69k | 10.48× | 87.4% | write 1.223 ms |

The shard-isolated case deliberately assigns each worker a disjoint set of shard IDs. The ordinary disjoint-key case only partitions key ranges; hash collisions still make different workers contend for shards. This distinction explains the 10.23× versus 2.64× write speedup at 12 cores.

## Contention shapes and workload mix

All four storage modes were swept from 1 to 12 physical writers for every write shape. The table condenses those sweeps to their endpoints; intermediate rows and p50/p95/p99.9 are in the summary CSV. Buffered means a userspace stream flush to the kernel page cache, without fdatasync. Sync and GroupCommit include stable-storage synchronization calls.

| SET mode | Key distribution | 1 writer ops/s | 12 writers ops/s | 12-writer speedup | 12-writer p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| Memory | Shard-isolated | 12.29M | 125.77M | 10.23× | 0.23 µs |
| Memory | Disjoint key ranges | 14.25M | 37.66M | 2.64× | 1.85 µs |
| Memory | Uniform shared | 14.29M | 31.40M | 2.20× | 2.30 µs |
| Memory | Zipf s=0.99 | 17.06M | 25.39M | 1.49× | 5.37 µs |
| Memory | One hot shard | 13.88M | 2.46M | 0.18× | 27.63 µs |
| Memory | One hot key | 32.76M | 6.03M | 0.18× | 17.71 µs |
| Buffered | Shard-isolated | 687.05k | 414.32k | 0.60× | 151.56 µs |
| Buffered | Disjoint key ranges | 721.04k | 461.46k | 0.64× | 123.56 µs |
| Buffered | Uniform shared | 721.91k | 462.32k | 0.64× | 119.37 µs |
| Buffered | Zipf s=0.99 | 727.07k | 462.88k | 0.64× | 154.60 µs |
| Buffered | One hot shard | 714.89k | 385.51k | 0.54× | 615.87 µs |
| Buffered | One hot key | 765.68k | 452.57k | 0.59× | 579.73 µs |
| Sync | Shard-isolated | 3.10k | 3.06k | 0.99× | 0.419 ms |
| Sync | Disjoint key ranges | 3.09k | 2.93k | 0.95× | 105.77 ms |
| Sync | Uniform shared | 3.07k | 2.95k | 0.96× | 110.22 ms |
| Sync | Zipf s=0.99 | 3.04k | 2.94k | 0.97× | 51.76 ms |
| Sync | One hot shard | 3.08k | 3.03k | 0.99× | 0.422 ms |
| Sync | One hot key | 3.01k | 3.03k | 1.01× | 0.423 ms |
| GroupCommit | Shard-isolated | 2.43k | 28.33k | 11.67× | 0.521 ms |
| GroupCommit | Disjoint key ranges | 2.45k | 25.89k | 10.59× | 0.952 ms |
| GroupCommit | Uniform shared | 2.45k | 25.69k | 10.48× | 1.223 ms |
| GroupCommit | Zipf s=0.99 | 2.43k | 20.70k | 8.50× | 3.06 ms |
| GroupCommit | One hot shard | 2.46k | 2.40k | 0.98× | 0.693 ms |
| GroupCommit | One hot key | 2.44k | 2.42k | 0.99× | 0.620 ms |

Hot-shard keys are chosen by hashing until they all map to shard 0. The single-hot-key case updates one key. Their lack of scaling is the expected cost of one shard lock and one logical mutation stream. GroupCommit cannot form large batches there because callers hold the shard lock through WAL acknowledgement. Buffered writes also flatten at the single stream mutex and flush path, even for shard-isolated keys.

Pure GET scaling was measured with all three required key distributions:

| GET keys | 1 reader ops/s | 12 readers ops/s | Speedup | 12-reader p99 |
| --- | ---: | ---: | ---: | ---: |
| Uniform shared | 14.36M | 88.88M | 6.19× | 0.37 µs |
| Zipf s=0.99 | 17.22M | 89.91M | 5.22× | 0.51 µs |
| One hot key | 34.04M | 34.59M | 1.02× | 1.98 µs |

A single hot read key still takes a shared shard lock; the lock's reader state becomes a contended cache line. This is a read-path limit of the current design, despite shared locking allowing simultaneous readers.

Every mixed ratio was swept at 1, 2, 4, 8, and 12 physical workers with uniform and Zipf keys. Aggregate throughput and separate read/write tails at 12 cores are:

| Mix | Keys | 1 worker ops/s | 12 workers ops/s | Speedup | 12-core read p99 | 12-core write p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 95R/5W | Uniform | 14.08M | 64.77M | 4.60× | 0.78 µs | 7.25 µs |
| 95R/5W | Zipf | 16.81M | 29.76M | 1.77× | 7.83 µs | 49.82 µs |
| 90R/10W | Uniform | 13.76M | 52.66M | 3.83× | 0.92 µs | 8.38 µs |
| 90R/10W | Zipf | 16.50M | 20.02M | 1.21× | 7.66 µs | 78.81 µs |
| 70R/30W | Uniform | 13.10M | 34.19M | 2.61× | 3.29 µs | 9.50 µs |
| 70R/30W | Zipf | 15.59M | 11.17M | 0.72× | 7.81 µs | 72.94 µs |
| 50R/50W | Uniform | 12.69M | 28.23M | 2.22× | 4.51 µs | 9.09 µs |
| 50R/50W | Zipf | 15.07M | 9.89M | 0.66× | 7.75 µs | 54.60 µs |

## GroupCommit and the sync boundary

The default batch limit is 32 records and the wait window is 20 µs. The observed batch size stays below the limit because the concurrent writer count is smaller. Batch p95/p99 are percentiles across physical sync batches, not across writes.

| Writers | Placement | Writes/s | Avg batch | Batch p95 / p99 | fdatasync/s | Writes/sync | Write p50 / p95 / p99 / p99.9 |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | Physical | 2,450 | 1.00 | 1 / 1 | 2,450 | 1.00 | 404 / 437 / 450 / — µs |
| 2 | Physical | 4,809 | 1.99 | 2 / 2 | 2,420 | 1.99 | 410 / 444 / 651 / — µs |
| 4 | Physical | 9,400 | 3.92 | 4 / 4 | 2,395 | 3.92 | 413 / 462 / 822 / — µs |
| 8 | Physical | 17,983 | 7.54 | 8 / 8 | 2,387 | 7.54 | 414 / 805 / 891 / — µs |
| 12 | Physical | 25,686 | 10.93 | 12 / 12 | 2,350 | 10.93 | 420 / 837 / 1,223 / 1,674 µs |
| 24 | SMT | 44,412 | 19.73 | 22 / 23 | 2,253 | 19.73 | 442 / 940 / 1,416 / 2,365 µs |

At 12 physical writers, GroupCommit is 8.69× faster than Sync for uniform SET and makes about 2,350 sync calls/s instead of roughly one per write. Its one-writer p99 is 450 µs versus Sync's 372 µs, a 78 µs price for queueing and handoff when no batch is available. At 12 writers it reduces the Sync queue's 110 ms p99 to 1.22 ms, although GroupCommit's own p99 rises with concurrency. The full [batch-delay sweep](benchmark_artifacts/group_delay_final_20260917/summary.csv) and [raw repetitions](benchmark_artifacts/group_delay_final_20260917/raw.csv) show:

| Delay | 12-writer writes/s | Write p50 / p95 / p99 | Avg batch | fdatasync/s |
| ---: | ---: | --- | ---: | ---: |
| 0 µs | 22,412 | 382 / 1,002 / 1,394 µs | 7.78 | 2,906 |
| 20 µs | 26,000 | 415 / 828 / 1,201 µs | 10.92 | 2,378 |
| 200 µs | 18,053 | 602 / 1,200 / 1,760 µs | 10.96 | 1,647 |
| 1,000 µs | 7,711 | 1,408 / 2,816 / 4,183 µs | 10.92 | 706 |

The 20 µs window improved both throughput and p99 relative to 0 µs at 12 writers, while increasing p50 by 33 µs. Longer windows delayed batches without growing them materially.

## Physical cores versus SMT

These 24-worker runs use two application threads per physical core. They are excluded from physical-core speedup claims.

| Workload | 12 physical ops/s | 24 SMT ops/s | Throughput change | p99 at 12 | p99 at 24 |
| --- | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 88.88M | 137.07M | +54.2% | 0.37 µs | 0.51 µs |
| SET uniform, memory | 31.40M | 41.34M | +31.7% | 2.30 µs | 4.08 µs |
| 70R/30W uniform, memory | 34.19M | 34.92M | +2.1% | write 9.50 µs | write 28.06 µs |
| GroupCommit SET uniform | 25.69k | 44.41k | +72.9% | 1.22 ms | 1.42 ms |

SMT helps reads and pure writes, but mixed 70R/30W gains only 2.1% while write p99 nearly triples. GroupCommit gains by enlarging batches from 10.93 to 19.73 writes/sync while sync calls fall from 2,350 to 2,253/s; SMT did not make the storage sync itself faster.

## Coarse-lock before and sharded after

The preserved pre-refactor source at 1f3ec22870fa1b9ea130659ca6df22552991a5e0 was rebuilt with the same GCC 13.3 toolchain and benchmark harness. Its [315 raw runs](benchmark_artifacts/coarse_gcc13_20260917/raw.csv), [105-row summary](benchmark_artifacts/coarse_gcc13_20260917/summary.csv), and [metadata](benchmark_artifacts/coarse_gcc13_20260917/metadata.json) are the compiler-matched baseline. The initial GCC 12 measurement is preserved separately under [coarse_gcc12_20260917](benchmark_artifacts/coarse_gcc12_20260917/) and is not used for the comparison below.

| Workload | 1 worker: coarse / sharded | 12 workers: coarse / sharded | 12-worker change |
| --- | ---: | ---: | ---: |
| GET uniform | 14.67M / 14.36M | 24.69M / 88.88M | 3.60× |
| GET Zipf | 18.07M / 17.22M | 31.23M / 89.91M | 2.88× |
| SET disjoint key ranges | 14.31M / 14.25M | 2.17M / 37.66M | 17.38× |
| SET uniform | 14.34M / 14.29M | 1.93M / 31.40M | 16.24× |
| SET hot shard | 14.17M / 13.88M | 2.49M / 2.46M | 0.99× |
| 70R/30W uniform | 13.99M / 13.10M | 1.54M / 34.19M | 22.24× |
| 70R/30W Zipf | 17.04M / 15.59M | 1.63M / 11.17M | 6.85× |
| Buffered SET uniform | 0.72M / 0.72M | 0.40M / 0.46M | 1.16× |

The 1-worker 70R/30W uniform result regressed about 6%, and 1-worker Zipf mixed about 9%; sharding adds routing and per-shard layout cost. The 12-worker coarse GET uniform baseline was noisy (19% coefficient of variation across three repetitions), but its range of 18.24–26.74M remains far below the sharded median of 88.88M. Median coefficient of variation across physical rows was 0.37% for the final sweep and 1.90% for the coarse sweep.

## Checkpoint latency

The same 20,000-SET harness used 1,000 preloaded keys, a snapshot on ext4, CPU 0, and five repetitions for each implementation. It excludes a WAL so the foreground snapshot trigger is isolated. Median results from [checkpoint raw rows](benchmark_artifacts/checkpoint_20260917/raw.csv):

| Implementation | Ops/s | SET p50 | p99 | p99.9 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| Coarse, synchronous snapshot | 3.16M | 50 ns | 80 ns | 7,000 ns | 273,113 ns |
| Sharded, background snapshot | 8.88M | 50 ns | 80 ns | 110 ns | 235,072 ns |

Moving snapshot serialization and I/O out of the foreground eliminates the recurring p99.9 cliff in this workload. A background checkpoint still briefly locks all shards to copy the map, so isolated maximum pauses remain; this design does not promise zero foreground interference.

## Perf and bottleneck evidence

The [perf stat files](benchmark_artifacts/perf_20260917/) cover 1, 6, and 12 pinned workers for uniform GET, shard-isolated SET, hot-shard SET, uniform 70R/30W, Sync SET, and GroupCommit SET. Each profile runs one second; counters include process setup and preload. Generic cache misses are available, but a reliable dedicated LLC-load event was not exposed on this host, so cache misses are not labeled as an LLC miss rate. Representative counters:

| Workload | T | Cycles B | Instructions B | IPC | Cache misses M | Context switches | CPU migrations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 1 | 4.81 | 6.35 | 1.32 | 77.4 | 15 | 2 |
| GET uniform | 6 | 28.30 | 21.24 | 0.75 | 309.5 | 52 | 9 |
| GET uniform | 12 | 56.27 | 39.67 | 0.71 | 591.7 | 149 | 17 |
| SET shard-isolated | 1 | 4.77 | 6.19 | 1.30 | 86.3 | 20 | 2 |
| SET shard-isolated | 6 | 27.95 | 34.44 | 1.23 | 482.5 | 67 | 7 |
| SET shard-isolated | 12 | 55.80 | 67.77 | 1.21 | 935.7 | 102 | 18 |
| SET hot-shard | 1 | 4.80 | 7.32 | 1.52 | 76.2 | 13 | 1 |
| SET hot-shard | 6 | 19.32 | 7.12 | 0.37 | 41.1 | 223,932 | 9 |
| SET hot-shard | 12 | 36.41 | 11.98 | 0.33 | 66.3 | 491,242 | 16 |
| 70R/30W uniform | 1 | 4.77 | 6.05 | 1.27 | 71.0 | 15 | 3 |
| 70R/30W uniform | 6 | 20.65 | 11.35 | 0.55 | 161.9 | 124,731 | 6 |
| 70R/30W uniform | 12 | 27.84 | 14.84 | 0.53 | 190.7 | 336,848 | 13 |
| Sync SET uniform | 1 | 0.32 | 0.26 | 0.82 | 6.1 | 6,051 | 1 |
| Sync SET uniform | 6 | 0.42 | 0.36 | 0.85 | 7.4 | 9,032 | 6 |
| Sync SET uniform | 12 | 0.45 | 0.40 | 0.89 | 8.5 | 8,843 | 12 |
| GroupCommit SET uniform | 1 | 0.51 | 0.37 | 0.72 | 8.8 | 14,042 | 2 |
| GroupCommit SET uniform | 6 | 0.88 | 0.63 | 0.72 | 11.0 | 25,082 | 8 |
| GroupCommit SET uniform | 12 | 1.40 | 0.96 | 0.68 | 15.2 | 41,045 | 14 |

The [read-only perf call-stack report](benchmark_artifacts/hotspots_20260917/uniform.report.txt) attributes about half its samples to pthread_rwlock_rdlock and another 10% to unlock. Read scaling loses IPC as lock and cache-line traffic rise, while context switches remain low. The [hot-shard call-stack report](benchmark_artifacts/hotspots_20260917/hot-shard.report.txt) shows pthread_rwlock_wrlock and kernel scheduling paths; 491k context switches and IPC 0.33 at 12 workers identify shard-lock contention rather than useful parallel work. Mixed uniform writes similarly drive 337k context switches at 12 workers. CPU migrations stay low; pinned application workers do not explain the flattening.

Sync writes plateau at about 3,000 fdatasync calls/s and use little CPU; the storage synchronization boundary limits throughput. GroupCommit remains near 2,350 sync calls/s at 12 workers but acknowledges 10.93 writes per call. The [strace syscall counts](benchmark_artifacts/perf_20260917/group_write_uniform_syscalls.txt) show futex wake/wait activity in the batch path; tracing perturbs timings, so the in-process batch/sync counters above are the throughput evidence.

## Validation and limits

Remote Release CTest, ThreadSanitizer CTest, and ASan/UBSan CTest each passed 90/90. ThreadSanitizer required a no-ASLR container launch and disabled its lock-order tracker because the runtime could not initialize with this Linux memory layout and could not track 64 simultaneously held shard locks; data-race instrumentation remained enabled. The [validation logs](benchmark_artifacts/validation_20260917/) preserve the runs. Repeated high-contention recovery stress is part of the suite.

These are one-machine, one-day measurements, not fixed-frequency or power-loss tests. The WAL v2 format still uses native-endian integer fields and has no on-disk generation or sequence number. Automatic checkpoints keep WAL history until explicit compaction. The default CLI uses Buffered WAL; only Sync and GroupCommit have the documented fdatasync acknowledgement boundary. The older EC2 microbenchmarks and their different machine/semantics are in [Benchmark History](Benchmark_History.md).
