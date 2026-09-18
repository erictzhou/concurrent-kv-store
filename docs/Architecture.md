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
| Buffered | Write one v3 frame and flush the C++ stream. | Bytes reached the kernel page cache; no stable-storage sync was requested. |
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

New WAL files use a v3 header and frames. All integer fields below are
explicit little-endian; the file header and each frame have separate CRC32
checksums:

```text
[4-byte magic KVW3][le32 version=3][le32 endian marker=0x01020304]
[le64 generation][le32 header CRC32]

[le32 record length][le32 CRC32(sequence + payload)]
[le64 sequence][uint8 opcode][le32 key size][key]
SET also carries [le32 value size][value]
```

The writer assigns sequence numbers in physical WAL write order, starting at
one within each generation. Rotation and clear start a new generation. A
record, including its sequence, is bounded to 64 MiB; GroupCommit also bounds
total batch bytes. Replay validates header, framing, opcode, lengths,
checksum, and contiguous sequence numbers before changing the recovery map.
It stops at the first bad or incomplete frame and can truncate the untrusted
suffix to the last validated offset. A WAL with a corrupt tail rejects new
appends until the tail is explicitly recovered or truncated. Existing v2
files remain readable and appendable; rotation upgrades them to v3. v2
retains its historical native-endian format. The exact byte layout and
recovery rules are in [WAL Format](WAL_Format.md).

Callers prepare frame storage before entering the WAL writer lock. The writer
fills the next sequence and its CRC32 under that lock, so the checksum covers
the actual on-disk order without holding the lock for frame allocation.

Per-object failure hooks in WAL and Snapshot support deterministic tests at
write-before-sync, sync completion, snapshot temp write, temp sync, rename,
directory sync, and WAL rotation boundaries. A failed call never mutates the
in-memory store, although a complete WAL record may still be replayed after
an uncertain I/O completion.

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

- Legacy WAL v2 and snapshot v1 integer fields use native byte order.
  Snapshots do not record the WAL generation, so copying unrelated
  snapshot/WAL files together is unsupported even though new WAL files
  carry generation metadata.
- Automatic checkpoints copy the full map while briefly holding all shard
  locks. Foreground writers do not perform the copy, but they can wait during
  capture. Automatic checkpoints retain WAL history; explicit compaction is
  needed to bound the log.
- A physical power-loss guarantee depends on the filesystem and storage
  device honoring `fdatasync`/`fsync`. The benchmark measures these calls on
  ext4, not forced power cuts.
- GroupCommit uses one ordered writer thread and a single WAL file. Durable
  throughput eventually flattens at that writer/filesystem boundary.
