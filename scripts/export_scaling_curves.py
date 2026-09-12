#!/usr/bin/env python3
"""Export the five required scaling curves from a summary.csv file."""

import argparse
import csv
from pathlib import Path


CURVES = (
    ("read_uniform", "memory", "100", "uniform"),
    ("write_shard_isolated", "memory", "0", "disjoint-shard"),
    ("mixed_70_uniform", "memory", "70", "uniform"),
    ("sync_write_uniform", "sync", "0", "uniform"),
    ("group_write_uniform", "group", "0", "uniform"),
)
FIELDS = ("workload", "placement", "threads", "physical_cores_used", "cpus",
          "ops_per_sec", "speedup", "parallel_efficiency", "ops_per_sec_min",
          "ops_per_sec_max", "ops_per_sec_cv", "read_p50_ns", "read_p95_ns",
          "read_p99_ns", "read_p999_ns", "write_p50_ns", "write_p95_ns",
          "write_p99_ns", "write_p999_ns", "average_batch_size",
          "fdatasync_calls_per_sec", "writes_per_sync")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with args.summary.open(newline="") as f:
        source = list(csv.DictReader(f))
    rows = []
    for name, mode, read_percent, distribution in CURVES:
        matches = [row for row in source if row["mode"] == mode and
                   row["read_percent"] == read_percent and
                   row["distribution"] == distribution]
        if not matches:
            raise ValueError(f"missing curve: {name}")
        for row in matches:
            rows.append({field: (name if field == "workload" else row.get(field, ""))
                         for field in FIELDS})
    with args.output.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
