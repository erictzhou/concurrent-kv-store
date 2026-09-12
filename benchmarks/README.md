# Benchmarks

`core_hot_path/kv_store_benchmark.cpp` retains the earlier Google Benchmark
microbenchmarks. Its `BM_DurableSetWithWalFlush` name predates explicit
durability policies: that row uses Buffered WAL and does **not** call
`fdatasync`.

`scaling/kv_store_scaling.cpp` measures concurrent throughput and sampled
latencies with workers pinned to explicit Linux CPU IDs. The Python runner
detects physical cores and writes both raw repetitions and median summary CSV
files:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kv_store_scaling_benchmark
python3 scripts/run_scaling_benchmarks.py \
  --binary build/kv_store_scaling_benchmark \
  --output benchmark_results/scaling --scope final \
  --duration-ms 500 --repetitions 3
```

The final scope covers 100% GET with uniform, Zipf `s=0.99`, and one hot key;
100% SET in memory, Buffered, Sync, and GroupCommit modes with disjoint key
ranges, shard-isolated ranges, uniform shared keys, one hot shard, one hot key,
and Zipf keys; and 95/5, 90/10, 70/30, and 50/50 read/write mixes with uniform
and Zipf keys. SMT results are marked separately. The GroupCommit WAL writer
is pinned to the first selected physical CPU; application workers each have
their own CPU assignment.

The suite reports ops/sec, speedup versus that workload's one-thread row,
parallel efficiency, physical cores used, worker CPU IDs, read/write p50,
p95, p99, and p99.9 when enough samples exist, and batch/sync metrics. Each
duration excludes preload and trace generation. All timed WAL files live on
the benchmark output filesystem, so publish its filesystem type with results.

`scripts/run_group_delay_sweep.py` compares batching windows of 0, 20, 200,
and 1,000 µs. `scaling/checkpoint_cliff.cpp` measures the maximum and tail
latency of automatic snapshot triggers. Publication data and perf findings
are in [docs/Benchmarks.md](../docs/Benchmarks.md).
