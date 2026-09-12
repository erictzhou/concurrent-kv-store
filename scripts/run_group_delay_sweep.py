#!/usr/bin/env python3
"""Measure the GroupCommit batching-window tradeoff on physical cores."""

import argparse
from pathlib import Path
import statistics
import subprocess

from run_scaling_benchmarks import cpu_topology, parse_output, write_csv


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration-ms", type=int, default=500)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    physical, _ = cpu_topology()
    rows = []
    for delay in (0, 20, 200, 1000):
        for count in (1, 4, len(physical)):
            cpus = physical[:count]
            for repetition in range(args.repetitions):
                command = [str(args.binary), "--mode", "group", "--distribution", "uniform",
                           "--read-percent", "0", "--threads", str(count), "--cpus",
                           ",".join(map(str, cpus)), "--duration-ms", str(args.duration_ms),
                           "--group-delay-us", str(delay), "--wal-path", str(args.output / "group.wal")]
                output = subprocess.check_output(command, text=True)
                row = parse_output(output)
                row.update({"repetition": repetition + 1, "physical_cores_used": count,
                            "cpus": ":".join(map(str, cpus))})
                rows.append(row)
                write_csv(args.output / "raw.csv", rows)
                print(delay, count, repetition + 1, row["ops_per_sec"], flush=True)
    summary = []
    for delay in (0, 20, 200, 1000):
        for count in (1, 4, len(physical)):
            group = [row for row in rows if int(row["group_delay_us"]) == delay and
                     int(row["threads"]) == count]
            result = {"group_delay_us": delay, "threads": count, "physical_cores_used": count,
                      "repetitions": len(group)}
            for metric in ("ops_per_sec", "write_p50_ns", "write_p95_ns", "write_p99_ns",
                           "write_p999_ns", "write_samples", "average_batch_size", "batch_p95",
                           "batch_p99", "fdatasync_calls_per_sec", "writes_per_sync"):
                result[metric] = statistics.median(float(row[metric]) for row in group)
            if result["write_samples"] < 10000:
                result["write_p999_ns"] = ""
            summary.append(result)
    write_csv(args.output / "summary.csv", summary)
    (args.output / "group.wal").unlink(missing_ok=True)


if __name__ == "__main__":
    main()
