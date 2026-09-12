# Architecture

## Components

```text
CLI -> KVStore -> 64 shard maps
                -> WriteAheadLog -> buffered / per-record sync / batch sync
                -> Snapshot (background checkpoint or explicit compaction)
```

The CLI runs in one process. `KVStore` provides the concurrent storage API.
Each key is routed by `std::hash<std::string>(key) & 63` to a shard containing
an `std::unordered_map` and a reader/writer lock. Shards are aligned to 64
bytes so their lock metadata does not share a cache line.

`Get` and `Contains` take one shard's shared lock. `Set` and `Delete` take one
shard's exclusive lock and retain it until the WAL policy acknowledges the
record. Holding the lock across WAL submission preserves the order of
mutations to the same key and prevents a read from observing an unacknowledged
write. Operations on unrelated shards may proceed concurrently. `Size` takes
all shard locks shared; clear, recovery, snapshot capture, and explicit
compaction take all shard locks exclusive in ascending index order. A separate
administrative lock serializes global operations, but is absent from the
per-key hot path.

Store copy/move is supported only without a configured Snapshot; a store with
a background checkpoint thread cannot be copied or moved. A WAL or Snapshot
object should not be shared across independent stores that perform
administrative operations concurrently.

## WAL acknowledgement policies

| Policy | WAL action before memory mutation | Successful acknowledgement means |
| --- | --- | --- |
| No WAL | None | In-memory change is visible. |
| Buffered | Write one v2 frame and flush the C++ stream. | Bytes reached the kernel page cache; no stable-storage sync was requested. |
| Sync | Write one frame, flush, and call `fdatasync`. | The filesystem synchronized that record before the call returned. |
| GroupCommit | An ordered writer joins up to `max_batch` frames, flushes, and calls `fdatasync` once. | The caller's batch completed its sync. |

GroupCommit defaults to `max_batch=32` and `max_delay_us=20`. Writers enqueue
framed records in order and sleep on a condition variable. The writer uses
one output write and one sync per batch, then wakes every waiter. On an output
or sync error it fails the affected batch and queued waiters, and later
submissions fail rather than silently succeeding. Sync and GroupCommit also
synchronize the WAL directory on opening a new WAL path; rotation synchronizes
the truncation before subsequent writes.

Same-shard memory order follows WAL submission order because the shard lock
spans submission and acknowledgement. Writes to different shards can become
visible in a different order after a shared group sync, but their WAL records
are both committed before either successful acknowledgement. Snapshot capture
waits for every shard operation to finish, so the recorded WAL offset covers
exactly the captured state.

The WAL v2 frame remains:

```text
[uint32 payload_length][uint32 crc32(payload)][payload bytes]
SET:    [uint8 op=1][uint32 key_size][key][uint32 value_size][value]
DELETE: [uint8 op=2][uint32 key_size][key]
```

Payloads are bounded to 64 MiB. Replay validates framing, opcode, lengths,
and checksum before changing the recovery map. It stops at the first bad or
incomplete frame and can truncate the untrusted suffix to the last validated
offset. The v2 format uses native integer byte order. It has no file header,
generation, or on-disk sequence number, so cross-endian portability and
generation-aware recovery are open format work.

## Checkpoints and recovery

After 1,000 foreground mutations, `KVStore` schedules one background
checkpoint. The scheduling step is bounded; the requesting write does not
serialize or write a snapshot. The background thread acquires every shard
lock, copies the map and the WAL end offset, resets the trigger counter, then
releases the shard locks. It writes the snapshot temporary file and verifies
it outside the foreground critical section. A snapshot temporary file is
flushed and synchronized before rename, and the parent directory is
synchronized after rename. Another trigger while a checkpoint is active is
coalesced into one further request. `WaitForCheckpoints()` waits for queued
work and reports any background failure. Later foreground writes fail at the
start if a background failure has been recorded.

Automatic checkpoints do not truncate the WAL. This leaves a valid recovery
prefix even when new writes continue while the snapshot is being published.
`CompactPersistence()` serializes with the background snapshot worker,
captures the full map, publishes a synchronized verified snapshot with WAL
offset zero, then rotates and synchronizes the WAL. A failed snapshot leaves
the WAL untouched. Explicit save and persistence reset also serialize with
background publication so an older checkpoint cannot replace a newer one.

Startup recovery loads the last valid snapshot, then replays the WAL from the
snapshot's covered byte offset. An absent snapshot starts replay at offset
zero. The default CLI currently constructs a Buffered WAL; successful CLI
writes can be lost after a power failure. Sync and GroupCommit callers have a
filesystem synchronization boundary. Failed or interrupted operations may
still leave a valid WAL record that recovery applies even though the caller
did not receive success, as is normal for uncertain I/O completion.

## Remaining limits

- WAL v2 integer fields use native byte order and lack generation/sequence
  metadata. The file format is not cross-endian portable.
- Automatic checkpoints copy the full map while briefly holding all shard
  locks. Foreground writers do not perform the copy, but they can wait during
  capture. Automatic checkpoints retain WAL history; explicit compaction is
  needed to bound the log.
- A physical power-loss guarantee depends on the filesystem and storage
  device honoring `fdatasync`/`fsync`. The benchmark measures these calls on
  ext4, not forced power cuts.
- GroupCommit uses one ordered writer thread and a single WAL file. Durable
  throughput eventually flattens at that writer/filesystem boundary.
