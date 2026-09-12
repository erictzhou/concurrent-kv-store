# Roadmap

## Completed foundations

- 0.3.x: C++20/CMake project, GoogleTest and Google Benchmark suites, CRC32
  WAL framing, bounded corruption handling.
- 0.4.x: Verified snapshots, WAL rotation, snapshot-assisted recovery, first
  single-thread EC2 baseline.
- 0.5.x: Coarse reader/writer lock and concurrent correctness tests.
- 0.6.0: 64 independent shard locks, Buffered/Sync/GroupCommit WAL policies,
  ordered batch writer, background automatic checkpoints, synchronized
  snapshot publication, and pinned physical-core benchmark sweeps.

## Remaining local-engine work

- Versioned, explicitly little-endian WAL format with generation and sequence
  numbers, plus an upgrade path for v2 files.
- Deterministic fault injection and process-kill cases at WAL sync, snapshot
  publication, and rotation boundaries.
- A bounded or segmented WAL cleanup strategy for automatic checkpoints.
- More checkpoint capture strategies if full-map copy pauses become material.
- CLI configuration for Sync and GroupCommit; the CLI currently uses Buffered.

SSTables, LSM compaction, networking, replication, Raft, SQL, and protocol
compatibility are outside the current local-engine scope.
