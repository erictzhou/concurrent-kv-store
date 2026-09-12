#!/usr/bin/env python3
"""Collect representative perf and syscall evidence for physical-core scaling."""

import argparse
from pathlib import Path
import subprocess

from run_scaling_benchmarks import cpu_topology


EVENTS = "cycles,instructions,cache-references,cache-misses,branches,branch-misses,context-switches,cpu-migrations"
CASES = (
    ("read_uniform", "memory", 100, "uniform"),
    ("write_disjoint_shard", "memory", 0, "disjoint-shard"),
    ("write_hot_shard", "memory", 0, "hot-shard"),
    ("mixed_70_uniform", "memory", 70, "uniform"),
    ("sync_write_uniform", "sync", 0, "uniform"),
    ("group_write_uniform", "group", 0, "uniform"),
)


def benchmark_command(binary, output, cpus, mode, read_percent, distribution):
    command = [str(binary), "--mode", mode, "--read-percent", str(read_percent),
               "--distribution", distribution, "--threads", str(len(cpus)),
               "--cpus", ",".join(map(str, cpus)), "--duration-ms", "1000"]
    if mode != "memory":
        command += ["--wal-path", str(output / "profile.wal")]
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    physical, _ = cpu_topology()
    counts = list(dict.fromkeys((1, max(1, len(physical) // 2), len(physical))))
    for name, mode, read_percent, distribution in CASES:
        for count in counts:
            cpus = physical[:count]
            command = benchmark_command(binary, args.output, cpus, mode,
                                        read_percent, distribution)
            perf_file = args.output / f"{name}_{count}t.perf.csv"
            result_file = args.output / f"{name}_{count}t.result.txt"
            with result_file.open("w") as f:
                subprocess.run(["perf", "stat", "-x,", "-e", EVENTS, "-o",
                                str(perf_file)] + command, stdout=f, check=True)
            print(name, count, flush=True)
    for name, mode in (("sync_write_uniform", "sync"),
                       ("group_write_uniform", "group")):
        command = benchmark_command(binary, args.output, physical, mode, 0, "uniform")
        with (args.output / f"{name}_syscalls.result.txt").open("w") as f:
            subprocess.run(["strace", "-f", "-c", "-e", "trace=futex,fdatasync,write",
                            "-o", str(args.output / f"{name}_syscalls.txt")] + command,
                           stdout=f, check=True)
    (args.output / "profile.wal").unlink(missing_ok=True)


if __name__ == "__main__":
    main()
