#!/usr/bin/env python3
"""Run pinned physical-core and separate SMT scaling sweeps on Linux."""

import argparse
import csv
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def cpu_topology():
    allowed = os.sched_getaffinity(0)
    cores = {}
    for cpu in sorted(allowed):
        base = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
        socket = int((base / "physical_package_id").read_text())
        core = int((base / "core_id").read_text())
        cache = Path(f"/sys/devices/system/cpu/cpu{cpu}/cache/index3/id")
        l3 = int(cache.read_text()) if cache.exists() else -1
        cores.setdefault((socket, core), {"l3": l3, "cpus": []})["cpus"].append(cpu)
    domains = {}
    for entry in cores.values():
        domains.setdefault(entry["l3"], []).append(entry["cpus"])
    ordered = []
    while any(domains.values()):
        for domain in sorted(domains):
            if domains[domain]:
                ordered.append(domains[domain].pop(0))
    return [core[0] for core in ordered], [core[1] for core in ordered if len(core) > 1]


def workloads(scope):
    cases = []
    for distribution in ("uniform", "zipf", "single-key"):
        cases.append(("memory", 100, distribution))
    write_distributions = ("disjoint", "uniform", "hot-shard", "single-key", "zipf")
    if scope == "final":
        write_distributions = ("disjoint-shard",) + write_distributions
    for distribution in write_distributions:
        for mode in (("memory", "buffered") if scope == "baseline" else
                     ("memory", "buffered", "sync", "group")):
            cases.append((mode, 0, distribution))
    for read_percent in (95, 90, 70, 50):
        for distribution in ("uniform", "zipf"):
            cases.append(("memory", read_percent, distribution))
    return cases


def parse_output(output):
    row = {}
    for item in output.strip().split(","):
        key, value = item.split("=", 1)
        row[key] = value
    return row


def run(binary, mode, read_percent, distribution, cpus, duration_ms, wal_path):
    command = [str(binary), "--mode", mode, "--read-percent", str(read_percent),
               "--distribution", distribution, "--threads", str(len(cpus)),
               "--cpus", ",".join(map(str, cpus)), "--duration-ms", str(duration_ms),
               "--zipf-s", "0.99"]
    if mode != "memory":
        command += ["--wal-path", str(wal_path)]
    result = subprocess.run(command, text=True, capture_output=True, check=True)
    return parse_output(result.stdout)


def write_csv(path, rows):
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def summarize(rows):
    groups = {}
    for row in rows:
        key = (row["placement"], row["mode"], row["read_percent"],
               row["distribution"], row["threads"])
        groups.setdefault(key, []).append(row)
    summaries = []
    metric_fields = ["ops_per_sec", "read_p50_ns", "read_p95_ns", "read_p99_ns",
                     "read_p999_ns", "write_p50_ns", "write_p95_ns",
                     "write_p99_ns", "write_p999_ns", "read_samples", "write_samples",
                     "average_batch_size", "batch_p95", "batch_p99",
                     "fdatasync_calls_per_sec", "writes_per_sync"]
    for key, group in groups.items():
        row = {name: value for name, value in zip(
            ("placement", "mode", "read_percent", "distribution", "threads"), key)}
        row["physical_cores_used"] = group[0]["physical_cores_used"]
        row["cpus"] = group[0]["cpus"]
        row["repetitions"] = len(group)
        for field in metric_fields:
            numbers = [float(item[field]) for item in group if item.get(field, "") != ""]
            row[field] = statistics.median(numbers) if numbers else ""
        for kind in ("read", "write"):
            if row[f"{kind}_samples"] == "" or row[f"{kind}_samples"] < 10000:
                row[f"{kind}_p999_ns"] = ""
        rates = [float(item["ops_per_sec"]) for item in group]
        row["ops_per_sec_min"] = min(rates)
        row["ops_per_sec_max"] = max(rates)
        row["ops_per_sec_cv"] = statistics.stdev(rates) / statistics.mean(rates) if len(rates) > 1 else 0
        summaries.append(row)
    baselines = {(row["mode"], row["read_percent"], row["distribution"]): row["ops_per_sec"]
                 for row in summaries if row["placement"] == "physical" and int(row["threads"]) == 1}
    for row in summaries:
        base = baselines[(row["mode"], row["read_percent"], row["distribution"])]
        row["speedup"] = row["ops_per_sec"] / base
        row["parallel_efficiency"] = row["speedup"] / int(row["threads"])
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scope", choices=("baseline", "final"), required=True)
    parser.add_argument("--duration-ms", type=int, default=500)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--only", help="comma-separated mode/read-percent/distribution filter")
    parser.add_argument("--source-commit", default="uncommitted")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    physical, siblings = cpu_topology()
    counts = [count for count in (1, 2, 4, 8, len(physical)) if count <= len(physical)]
    counts = list(dict.fromkeys(counts))
    if len(physical) < 1:
        raise RuntimeError("no physical cores available")
    metadata = {"date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "hostname": os.uname().nodename, "uname": " ".join(os.uname()),
                "physical_cpus": physical, "smt_siblings": siblings,
                "N_physical_cores": len(physical), "thread_counts": counts,
                "duration_ms": args.duration_ms, "repetitions": args.repetitions,
                "scope": args.scope, "zipf_s": 0.99,
                "source_commit": args.source_commit,
                "lscpu": subprocess.check_output(["lscpu"], text=True),
                "governor": Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read_text().strip(),
                "binary_sha256": subprocess.check_output(["sha256sum", str(args.binary)], text=True).split()[0]}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    cases = workloads(args.scope)
    if args.only:
        filters = args.only.split(",")
        cases = [case for case in cases if all(part in "/".join(map(str, case)) for part in filters)]
    rows = []
    for mode, read_percent, distribution in cases:
        placements = [("physical", physical[:count]) for count in counts]
        if args.scope == "final" and len(siblings) == len(physical) and (
            (mode == "memory" and (read_percent == 100 and distribution == "uniform" or
                                   read_percent == 0 and distribution == "uniform" or
                                   read_percent == 70 and distribution == "uniform")) or
            (mode == "group" and read_percent == 0 and distribution == "uniform")
        ):
            placements.append(("smt", physical + siblings))
        for placement, cpus in placements:
            for repetition in range(args.repetitions):
                result = run(args.binary, mode, read_percent, distribution, cpus,
                             args.duration_ms, args.output / "current.wal")
                result.update({"placement": placement, "physical_cores_used": min(len(cpus), len(physical)),
                               "cpus": ":".join(map(str, cpus)), "repetition": repetition + 1})
                rows.append(result)
                write_csv(args.output / "raw.csv", rows)
                print(f"{mode:8} {read_percent:3}R {distribution:11} {placement:8} "
                      f"{len(cpus):2}t rep{repetition + 1}: {float(result['ops_per_sec']):,.0f} ops/s", flush=True)
    write_csv(args.output / "summary.csv", summarize(rows))
    (args.output / "current.wal").unlink(missing_ok=True)


if __name__ == "__main__":
    main()
