# WAL format and recovery

New WAL files use version 3. Every multi-byte integer is unsigned and
little-endian. A file begins with a 24-byte header:

| Byte offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic KVW3 |
| 4 | 4 | Format version, currently 3 |
| 8 | 4 | Byte-order marker, 0x01020304 |
| 12 | 8 | WAL generation, starting at 1 |
| 20 | 4 | CRC32 of header bytes 0 through 19 |

Each mutation then has an eight-byte frame prefix followed by a record:

| Field | Width | Meaning |
| --- | ---: | --- |
| Record length | 4 | Byte count of sequence plus payload; maximum 64 MiB |
| Record CRC32 | 4 | Checksum of the entire record, excluding frame prefix |
| Sequence | 8 | Monotonic number within this generation, starting at 1 |
| Opcode | 1 | 1 = SET; 2 = DELETE |
| Key length | 4 | Byte count of key |
| Key | Variable | Raw key bytes |
| Value length | 4 | Present for SET only |
| Value | Variable | Present for SET only |

CRC32 uses the reflected 0xEDB88320 polynomial, initial state 0xFFFFFFFF,
and final complement. The sequence is covered by the record checksum. The
ordered WAL writer assigns sequences while constructing frames under the WAL
I/O mutex. GroupCommit assigns consecutive numbers within each batch in
the same order that the frames are written. Generation increments on clear
or rotation; sequence resets to 1.

Replay verifies the file header before reading frames. It validates bounded
length, complete framing, checksum, opcode, key/value lengths, and contiguous
sequences before applying each mutation to the recovery map. On a bad or
incomplete frame, it stops at the last verified byte offset. A recovered WAL
with an untrusted tail refuses new appends until the caller explicitly
truncates to that offset. Replay from a snapshot offset validates the file
header, then begins at the stored byte offset. A fresh replay from offset 0
starts immediately after the 24-byte header.

The earlier v2 format has no file header or sequence. Its records are a
native-endian 32-bit payload length, native-endian CRC32, and an opcode plus
native-endian key/value lengths. Existing v2 files remain readable and
appendable in their original format. A clear or rotation starts a new v3
generation; opening a v2 file does not silently rewrite it.

The snapshot format is still native-endian and records a WAL byte offset,
not a WAL generation. Supported checkpoint and compaction operations
coordinate these files under store locks: automatic checkpoints retain WAL
history, and explicit compaction publishes a verified snapshot with offset
zero before rotating the WAL. Copying unrelated snapshot and WAL files
together is unsupported.

SET and DELETE append to the WAL before changing the in-memory shard. A
Buffered acknowledgement means the C++ stream was flushed into the kernel
page cache. A Sync acknowledgement follows fdatasync for that record;
GroupCommit acknowledgement follows fdatasync for its batch. If a write or
sync fails after a complete frame was written, the caller receives an error
and memory is not changed, but recovery may still apply that frame. This is
an uncertain commit outcome, not evidence that the failed call was
acknowledged.
