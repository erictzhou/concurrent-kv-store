# Multicore benchmarks — Ryzen 9 5900X

The sequenced-WAL v3 engine reaches 125.52M in-memory SET/s when writers own separate shards: 10.29× versus one core and 85.7% parallel efficiency. Uniform shared writes reach 31.38M; one hot shard falls to 2.48M. Sync WAL stays near 2.96k writes/s, while GroupCommit reaches 25.68k writes/s with 10.97 writes per sync. These are distinct storage modes, not interchangeable throughput numbers.

## Machine, provenance, and method

| Item | Recorded value |
| --- | --- |
| Host | `agency-bench`, AMD Ryzen 9 5900X, Linux 7.0.0-31-generic |
| Topology | N = 12 physical cores, 24 logical CPUs, one NUMA node, two 32 MiB L3 domains |
| Physical worker CPU order | 0, 6, 1, 7, 2, 8, 3, 9, 4, 10, 5, 11 |
| SMT sibling order | 12, 18, 13, 19, 14, 20, 15, 21, 16, 22, 17, 23 |
| CPU state | powersave governor, boost enabled; no fixed-frequency isolation |
| WAL output | Home-directory ext4 on `/dev/nvme1n1p2`; `/tmp` is tmpfs and was excluded |
| Build | Ubuntu 24.04 container, GCC 13.3.0, CMake 3.28.3, Release (`-O3 -DNDEBUG`) |
| Tested source commit | `d040d98f22c0edfacae78958222e4b4c0e35d318` |
| Scaling binary SHA-256 | `594ef3eee07d5a54a1880dd3c6239104a329bdc6f3b20db8e4bfc410e232065b` |
| Pre-run validation | Remote Release CTest: 102/102 passed |
| Repetitions | Three adjacent 500 ms runs per point; tables use the median |
| Physical sweep | 1, 2, 4, 8, 12 workers; one application worker per distinct physical core |
| SMT comparison | 24 application workers on 12 cores; separately labeled |
| Working sets | 10,000 shared keys or 10,000 thread-owned keys per worker; Zipf exponent s = 0.99 |
| Latency sampling | Every 32nd memory op, every 8th Buffered op, every Sync/GroupCommit op |
| Tail rule | p99.9 omitted when median sample count is below 10,000 |

Application workers use pthread affinity. The GroupCommit writer is pinned to CPU 0, sharing that CPU with the first application worker. Physical CPUs alternate across the two L3 domains. Key generation and preloading finish before timing. The timed operation mix is entirely GET, entirely SET, or the specified read/write ratio. Throughput is completed operations divided by elapsed wall time.

The remote Release build, full sweep, perf runs, delay sweep, and sanitizer
jobs ran in windows of the persistent `kvstore` tmux session. The session was
reused and left running after the jobs completed.

Speedup(T) = throughput(T) / throughput(1); parallel efficiency(T) = speedup(T) / T. The [537 raw repetitions](benchmark_artifacts/final_v3_opt_20260917/raw.csv), [179-row median summary](benchmark_artifacts/final_v3_opt_20260917/summary.csv), [28-row plot-ready curves](benchmark_artifacts/final_v3_opt_20260917/plot_curves.csv), and [machine metadata](benchmark_artifacts/final_v3_opt_20260917/metadata.json) provide thread counts, physical cores used, CPU IDs, throughput, speedup, efficiency, p50/p95/p99/p99.9, WAL batch/sync rates, and run-to-run variation. Read and write latency columns are separate. SMT rows are marked separately and excluded from physical-core scaling claims.

The median coefficient of variation in throughput across physical-core rows
was 0.49% (95th percentile 1.73%). The machine was not frequency-isolated;
individual tail percentiles, especially short one-writer durable runs, are
less stable than the throughput medians.

## Physical-core scaling curves

The CSV curves are suitable for plotting throughput, speedup, efficiency, and read/write p99 against thread count. Latencies below name their operation; the mixed curve shows read and write tails separately. Full percentile columns are in the summary CSV.

| Workload | Threads / cores | Ops/s | Speedup | Efficiency | p99 latency |
| --- | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 1 / 1 | 14.38M | 1.00× | 100.0% | read 0.12 µs |
| GET uniform | 2 / 2 | 17.96M | 1.25× | 62.4% | read 0.23 µs |
| GET uniform | 4 / 4 | 33.12M | 2.30× | 57.6% | read 0.29 µs |
| GET uniform | 8 / 8 | 61.34M | 4.27× | 53.3% | read 0.36 µs |
| GET uniform | 12 / 12 | 88.98M | 6.19× | 51.6% | read 0.37 µs |
| SET shard-isolated, memory | 1 / 1 | 12.20M | 1.00× | 100.0% | write 0.14 µs |
| SET shard-isolated, memory | 2 / 2 | 23.18M | 1.90× | 95.0% | write 0.18 µs |
| SET shard-isolated, memory | 4 / 4 | 45.16M | 3.70× | 92.5% | write 0.20 µs |
| SET shard-isolated, memory | 8 / 8 | 87.25M | 7.15× | 89.4% | write 0.22 µs |
| SET shard-isolated, memory | 12 / 12 | 125.52M | 10.29× | 85.7% | write 0.23 µs |
| 70R/30W uniform, memory | 1 / 1 | 13.11M | 1.00× | 100.0% | read 0.13 µs / write 0.14 µs |
| 70R/30W uniform, memory | 2 / 2 | 12.42M | 0.95× | 47.3% | read 0.38 µs / write 0.96 µs |
| 70R/30W uniform, memory | 4 / 4 | 20.24M | 1.54× | 38.6% | read 0.81 µs / write 1.29 µs |
| 70R/30W uniform, memory | 8 / 8 | 29.80M | 2.27× | 28.4% | read 0.97 µs / write 6.30 µs |
| 70R/30W uniform, memory | 12 / 12 | 34.20M | 2.61× | 21.7% | read 3.34 µs / write 9.50 µs |
| SET Sync WAL | 1 / 1 | 3.03k | 1.00× | 100.0% | write 409.70 µs |
| SET Sync WAL | 2 / 2 | 2.98k | 0.98× | 49.2% | write 16.484 ms |
| SET Sync WAL | 4 / 4 | 2.98k | 0.98× | 24.5% | write 43.611 ms |
| SET Sync WAL | 8 / 8 | 2.95k | 0.97× | 12.2% | write 78.743 ms |
| SET Sync WAL | 12 / 12 | 2.96k | 0.98× | 8.1% | write 102.121 ms |
| SET GroupCommit WAL | 1 / 1 | 2.43k | 1.00× | 100.0% | write 650.83 µs |
| SET GroupCommit WAL | 2 / 2 | 4.78k | 1.97× | 98.4% | write 788.39 µs |
| SET GroupCommit WAL | 4 / 4 | 9.48k | 3.90× | 97.5% | write 817.70 µs |
| SET GroupCommit WAL | 8 / 8 | 17.94k | 7.38× | 92.2% | write 897.52 µs |
| SET GroupCommit WAL | 12 / 12 | 25.68k | 10.56× | 88.0% | write 1.201 ms |

Shard-isolated keys are chosen so each worker owns separate shard IDs. Disjoint key ranges alone still hash into overlapping shards: at 12 writers their 2.65× speedup is far below the 10.29× shard-isolated result. Uniform GET continues to gain through 12 cores, but its efficiency is 51.6% and its p99 rises from 0.12 µs to 0.37 µs.

## Write contention shapes and storage modes

Every mode and key distribution below was swept at 1, 2, 4, 8, and 12 physical writers. The table shows endpoints; intermediate results and all percentiles are in the summary CSV. Buffered flushes into the kernel page cache without `fdatasync`; Sync and GroupCommit include that call before acknowledgement.

| SET mode | Key distribution | 1 writer ops/s | 12 writers ops/s | 12-writer speedup | 12-writer p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Memory | Shard-isolated | 12.20M | 125.52M | 10.29× | 0.23 µs |
| Memory | Disjoint key ranges | 14.28M | 37.84M | 2.65× | 1.84 µs |
| Memory | Uniform shared | 14.30M | 31.38M | 2.19× | 2.33 µs |
| Memory | Zipf s=0.99 | 17.15M | 25.24M | 1.47× | 5.41 µs |
| Memory | One hot shard | 13.84M | 2.48M | 0.18× | 27.59 µs |
| Memory | One hot key | 32.55M | 5.92M | 0.18× | 17.72 µs |
| Buffered | Shard-isolated | 766.37k | 411.51k | 0.54× | 319.89 µs |
| Buffered | Disjoint key ranges | 790.52k | 405.60k | 0.51× | 141.92 µs |
| Buffered | Uniform shared | 786.22k | 406.66k | 0.52× | 151.06 µs |
| Buffered | Zipf s=0.99 | 787.94k | 410.56k | 0.52× | 191.09 µs |
| Buffered | One hot shard | 792.75k | 418.52k | 0.53× | 846.44 µs |
| Buffered | One hot key | 817.37k | 464.86k | 0.57× | 516.72 µs |
| Sync | Shard-isolated | 3.06k | 3.06k | 1.00× | 584.57 µs |
| Sync | Disjoint key ranges | 3.08k | 2.96k | 0.96× | 95.304 ms |
| Sync | Uniform shared | 3.03k | 2.96k | 0.98× | 102.121 ms |
| Sync | Zipf s=0.99 | 3.07k | 2.93k | 0.96× | 47.689 ms |
| Sync | One hot shard | 3.11k | 3.04k | 0.98× | 1.236 ms |
| Sync | One hot key | 3.03k | 3.05k | 1.01× | 412.76 µs |
| GroupCommit | Shard-isolated | 2.43k | 27.98k | 11.50× | 517.38 µs |
| GroupCommit | Disjoint key ranges | 2.45k | 25.68k | 10.50× | 1.172 ms |
| GroupCommit | Uniform shared | 2.43k | 25.68k | 10.56× | 1.201 ms |
| GroupCommit | Zipf s=0.99 | 2.45k | 20.69k | 8.45× | 3.212 ms |
| GroupCommit | One hot shard | 2.44k | 2.41k | 0.99× | 667.41 µs |
| GroupCommit | One hot key | 2.43k | 2.39k | 0.99× | 624.39 µs |

Hot-shard keys are found by hashing until they all map to shard 0. The single-hot-key case updates one key. Both serialize on one shard lock; GroupCommit cannot form large batches there because each caller holds that lock until WAL acknowledgement. Buffered writes flatten even for isolated shards because all records pass through one stream mutex and flush path.

Pure GET was measured under all three requested key distributions:

| GET keys | 1 reader ops/s | 12 readers ops/s | Speedup | 12-reader p99 |
| --- | ---: | ---: | ---: | ---: |
| Uniform shared | 14.38M | 88.98M | 6.19× | 0.37 µs |
| Zipf s=0.99 | 17.10M | 89.82M | 5.25× | 0.51 µs |
| One hot key | 34.00M | 34.81M | 1.02× | 1.95 µs |

A single hot read key still updates shared-lock reader state on one cache line. Its aggregate throughput is flat despite concurrent read permission.

Every mixed ratio was swept at 1, 2, 4, 8, and 12 physical workers with uniform and Zipf keys. The table reports aggregate throughput and distinct 12-core read/write tails:

| Mix | Keys | 1 worker ops/s | 12 workers ops/s | Speedup | Read p99 | Write p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 95R/5W | Uniform | 14.07M | 64.60M | 4.59× | 0.78 µs | 7.05 µs |
| 95R/5W | Zipf | 16.84M | 29.39M | 1.75× | 7.88 µs | 51.00 µs |
| 90R/10W | Uniform | 13.83M | 52.57M | 3.80× | 0.91 µs | 8.37 µs |
| 90R/10W | Zipf | 16.60M | 19.92M | 1.20× | 7.60 µs | 79.37 µs |
| 70R/30W | Uniform | 13.11M | 34.20M | 2.61× | 3.34 µs | 9.50 µs |
| 70R/30W | Zipf | 15.60M | 10.98M | 0.70× | 7.84 µs | 74.76 µs |
| 50R/50W | Uniform | 12.70M | 28.35M | 2.23× | 4.48 µs | 9.08 µs |
| 50R/50W | Zipf | 15.08M | 9.81M | 0.65× | 7.77 µs | 54.12 µs |

## GroupCommit and the sync boundary

The default batch limit is 32 records and the wait window is 20 µs. Batch p95/p99 are percentiles over physical sync batches. The one-writer row uses one writer, so no batching gain is possible.

| Writers | Placement | Writes/s | Avg batch | Batch p95 / p99 | fdatasync/s | Writes/sync | Write p50 / p95 / p99 / p99.9 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Physical | 2.43k | 1.00 | 1 / 1 | 2.43k | 1.00 | 405.69 µs / 435.44 µs / 650.83 µs / — |
| 2 | Physical | 4.78k | 1.99 | 2 / 2 | 2.41k | 1.99 | 405.44 µs / 450.99 µs / 788.39 µs / — |
| 4 | Physical | 9.48k | 3.90 | 4 / 4 | 2.43k | 3.90 | 408.56 µs / 457.62 µs / 817.70 µs / — |
| 8 | Physical | 17.94k | 7.56 | 8 / 8 | 2.39k | 7.56 | 409.87 µs / 767.70 µs / 897.52 µs / — |
| 12 | Physical | 25.68k | 10.97 | 12 / 12 | 2.35k | 10.97 | 418.35 µs / 834.75 µs / 1.201 ms / 1.676 ms |
| 24 | SMT | 44.56k | 19.73 | 22 / 23 | 2.26k | 19.73 | 444.46 µs / 937.19 µs / 1.417 ms / 2.269 ms |

At 12 physical writers, GroupCommit is 8.67× faster than Sync for uniform SET. It makes about 2.35k sync calls/s while acknowledging 10.97 writes per call. At one writer, GroupCommit p99 is 650.83 µs versus Sync's 409.70 µs, a queueing and handoff cost; the GroupCommit p99 ranged from 501.62 to 710.94 µs across its three short repetitions, so that one-writer tail estimate is noisy. At 12 writers, GroupCommit p99 is 1.201 ms against Sync's 102.121 ms; Sync's serialized queue produces a much worse tail.

The [batch-delay raw runs](benchmark_artifacts/group_delay_v3_opt_20260917/raw.csv) and [summary](benchmark_artifacts/group_delay_v3_opt_20260917/summary.csv) test four windows at 12 pinned writers:

| Delay | 12-writer writes/s | Write p50 / p95 / p99 | Avg batch | fdatasync/s |
| --- | ---: | ---: | ---: | ---: |
| 0 µs | 22.18k | 388.41 µs / 1.009 ms / 1.433 ms | 7.75 | 2.87k |
| 20 µs | 25.57k | 419.17 µs / 836.61 µs / 1.202 ms | 10.93 | 2.34k |
| 200 µs | 17.86k | 603.94 µs / 1.207 ms / 1.759 ms | 10.94 | 1.63k |
| 1000 µs | 7.71k | 1.409 ms / 2.826 ms / 4.188 ms | 10.90 | 706 |

The 20 µs window forms larger batches than 0 µs and raises p50 while improving throughput and p99 in this run. Longer delays do not materially increase batch size, so they mostly add latency and reduce throughput.

## Physical cores versus SMT

These 24-worker points use two application threads per physical core. Their speedup and efficiency are not mixed into the physical-core sweep.

| Workload | 12 physical ops/s | 24 SMT ops/s | Throughput change | p99 at 12 | p99 at 24 |
| --- | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 88.98M | 137.02M | +54.0% | 0.37 µs | 0.51 µs |
| SET uniform, memory | 31.38M | 41.20M | +31.3% | 2.33 µs | 4.26 µs |
| 70R/30W uniform, memory | 34.20M | 35.32M | +3.3% | 9.50 µs | 27.67 µs |
| GroupCommit SET uniform | 25.68k | 44.56k | +73.6% | 1.201 ms | 1.417 ms |

SMT adds throughput for pure reads and writes. The 70R/30W gain is only 3.3% while write p99 rises. GroupCommit gains by enlarging batches from 10.97 to 19.73 writes/sync; the sync device itself does not become faster.

## Coarse-lock before and sharded v3 after

The preserved pre-refactor source at `1f3ec22870fa1b9ea130659ca6df22552991a5e0` was rebuilt with the same GCC 13.3 toolchain and benchmark harness. Its [315 raw runs](benchmark_artifacts/coarse_gcc13_20260917/raw.csv), [105-row summary](benchmark_artifacts/coarse_gcc13_20260917/summary.csv), and [metadata](benchmark_artifacts/coarse_gcc13_20260917/metadata.json) are the compiler-matched baseline. This comparison spans the sharding and WAL v3 work. The earlier GCC 12 coarse run is preserved [separately](benchmark_artifacts/coarse_gcc12_20260917/).

| Workload | 1 worker: coarse / v3 | 12 workers: coarse / v3 | 12-worker change |
| --- | ---: | ---: | ---: |
| GET uniform | 14.67M / 14.38M | 24.69M / 88.98M | 3.60× |
| GET Zipf | 18.07M / 17.10M | 31.23M / 89.82M | 2.88× |
| SET disjoint key ranges | 14.31M / 14.28M | 2.17M / 37.84M | 17.46× |
| SET uniform | 14.34M / 14.30M | 1.93M / 31.38M | 16.23× |
| SET hot shard | 14.17M / 13.84M | 2.49M / 2.48M | 1.00× |
| 70R/30W uniform | 13.99M / 13.11M | 1.54M / 34.20M | 22.24× |
| 70R/30W Zipf | 17.04M / 15.60M | 1.63M / 10.98M | 6.73× |
| Buffered SET uniform | 717.13k / 786.22k | 398.81k / 406.66k | 1.02× |

The 1-worker mixed rows regress modestly from the coarse baseline, consistent with shard routing cost. The 12-worker coarse uniform GET row varied substantially (19% coefficient of variation across its three repetitions), but its range remains far below the sharded result. Buffered uniform SET at 12 workers is below both the coarse baseline and the earlier sharded v2 run; the controlled paired comparison and limits are discussed below.

## Checkpoint latency

The 20,000-SET harness uses 1,000 preloaded keys, an ext4 snapshot path, CPU 0, and five repetitions per implementation. It excludes WAL so the foreground snapshot trigger is isolated. The original [coarse runs](benchmark_artifacts/checkpoint_20260917/raw.csv) and final [v3 runs](benchmark_artifacts/checkpoint_v3_opt_20260917/raw.csv) are retained separately.

| Implementation | Ops/s | SET p50 | p99 | p99.9 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| Coarse, synchronous snapshot | 3.16M | 0.05 µs | 0.08 µs | 7.00 µs | 273.11 µs |
| Sharded v3, background snapshot | 10.04M | 0.05 µs | 0.08 µs | 0.10 µs | 245.48 µs |

Moving serialization and I/O off the foreground write eliminates the recurring p99.9 cliff in this harness. Snapshot capture still briefly locks all shards to copy a consistent map, so rare maximum pauses remain.

## Perf and bottleneck evidence

The [perf stat files](benchmark_artifacts/perf_v3_opt_20260917/) cover 1, 6, and 12 pinned workers for uniform GET, shard-isolated SET, hot-shard SET, uniform 70R/30W, Sync SET, and GroupCommit SET. Each profile runs one second; counters include setup and preload. Generic cache misses are available, but a reliable dedicated LLC-load event was not exposed, so cache misses are not labeled as an LLC miss rate.

| Workload | T | Cycles B | Instructions B | IPC | Cache misses M | Context switches | CPU migrations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| GET uniform | 1 | 4.84 | 6.39 | 1.32 | 77.2 | 12 | 2 |
| GET uniform | 6 | 28.32 | 21.16 | 0.75 | 307.1 | 58 | 7 |
| GET uniform | 12 | 56.28 | 39.58 | 0.70 | 591.9 | 132 | 13 |
| SET shard-isolated | 1 | 4.78 | 6.17 | 1.29 | 84.4 | 19 | 3 |
| SET shard-isolated | 6 | 28.02 | 34.30 | 1.22 | 476.8 | 54 | 8 |
| SET shard-isolated | 12 | 55.86 | 67.84 | 1.21 | 925.5 | 88 | 17 |
| SET hot shard | 1 | 4.80 | 7.25 | 1.51 | 75.5 | 15 | 2 |
| SET hot shard | 6 | 19.30 | 7.05 | 0.37 | 40.4 | 219,926 | 7 |
| SET hot shard | 12 | 36.41 | 12.04 | 0.33 | 66.5 | 494,257 | 14 |
| 70R/30W uniform | 1 | 4.77 | 6.05 | 1.27 | 71.3 | 12 | 2 |
| 70R/30W uniform | 6 | 20.36 | 11.09 | 0.54 | 157.7 | 126,483 | 7 |
| 70R/30W uniform | 12 | 28.00 | 14.80 | 0.53 | 187.7 | 338,176 | 11 |
| Sync SET uniform | 1 | 0.32 | 0.27 | 0.86 | 6.1 | 6,027 | 1 |
| Sync SET uniform | 6 | 0.41 | 0.37 | 0.90 | 8.3 | 8,883 | 5 |
| Sync SET uniform | 12 | 0.44 | 0.44 | 1.01 | 8.6 | 8,879 | 15 |
| GroupCommit SET uniform | 1 | 0.51 | 0.36 | 0.71 | 9.4 | 13,963 | 1 |
| GroupCommit SET uniform | 6 | 0.89 | 0.62 | 0.70 | 12.0 | 25,273 | 7 |
| GroupCommit SET uniform | 12 | 1.42 | 0.96 | 0.68 | 17.5 | 40,518 | 15 |

The [uniform GET sample report](benchmark_artifacts/hotspots_v3_opt_20260917/uniform.report.txt) attributes 46.72% of sampled cycles to `pthread_rwlock_rdlock` and 10.57% to unlock. IPC falls from 1.32 at one worker to 0.70 at 12, while context switching remains low: read scaling is chiefly lock/cache-line overhead, not sleeping. The [hot-shard sample report](benchmark_artifacts/hotspots_v3_opt_20260917/hot-shard.report.txt) includes write-lock and kernel samples; 494k context switches and IPC 0.33 at 12 workers identify severe shard-lock contention. Mixed uniform operations also drive 338k context switches and IPC 0.53. Application CPU migrations remain low.

Sync SET plateaus near 2.96k `fdatasync` calls/s and uses little CPU: the storage sync boundary limits it. GroupCommit acknowledges 10.97 writes per call while the sync rate is 2.35k/s. The [strace syscall counts](benchmark_artifacts/perf_v3_opt_20260917/group_write_uniform_syscalls.txt) show futex wait/wake activity in the batch path. Tracing perturbs timings, so the benchmark's in-process batch and sync counters are the throughput evidence.

## Validation, regressions, and limits

Remote Release CTest, ThreadSanitizer CTest, and ASan/UBSan CTest each passed 102/102 on the tested source. TSan used a no-ASLR container launch and disabled its lock-order tracker because the runtime could not initialize with this Linux memory layout and could not track 64 simultaneously held shard locks; data-race instrumentation remained enabled. The [validation logs](benchmark_artifacts/validation_v3_opt_20260917/) preserve the runs. New fault tests cover partial/corrupt v3 frames and header, sequence gaps, legacy v2 migration, WAL write/sync failures, snapshot publication failures, and abrupt process exit after Sync append.

Buffered uniform SET at 12 writers fell from 462.32k in the previous sharded v2 sweep to 406.66k here (-12.0%). The [controlled paired comparison](benchmark_artifacts/buffered_pair_v3_opt_20260917/) alternated five 500 ms runs of the v2 and optimized v3 binaries on the same host: v3 reached 789,845 versus 730,456 SET/s at one writer (+8.1%), but 422,027 versus 468,169 at 12 writers (-9.9%), with write p99 rising from 117.97 to 143.63 µs. Its required sequence and checksum finalization runs under the single WAL writer lock, a likely cause of the residual multicore gap. The [first v3 implementation](benchmark_artifacts/buffered_pair_v3_20260917/) was 21.4% slower than v2 at 12 writers in the same paired method; frame preparation and table-driven CRC32 reduced that gap. The single-worker mixed path also remains slower than the coarse-lock baseline. These regressions are retained rather than omitted.

These are one-machine, one-day measurements, not fixed-frequency or power-loss tests. New WAL v3 records are portable little-endian and sequenced; legacy v2 and snapshot v1 fields still use native byte order. Snapshots store a WAL byte offset but not a generation, so unrelated snapshot/WAL file pairs cannot be mixed safely. Automatic checkpoints keep WAL history until explicit compaction. A WAL write that fails after producing a complete record may be replayed despite the caller receiving an error; the documented acknowledgement contract does not promise rollback of uncertain I/O. The default CLI uses Buffered WAL; only Sync and GroupCommit have the `fdatasync` acknowledgement boundary. Earlier 5900X v2 results and different-machine EC2 microbenchmarks are in [Benchmark History](Benchmark_History.md).
