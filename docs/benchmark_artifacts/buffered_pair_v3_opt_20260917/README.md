# Buffered WAL v2 versus optimized v3

`agency-bench`, AMD Ryzen 9 5900X, GCC 13.3.0 Release binaries, separate WAL
files on home-directory ext4. The v2 source was
`596a72eb9c4df7ae037851acf30af471f48369c3`; optimized v3 was
`d040d98f22c0edfacae78958222e4b4c0e35d318`. Binary SHA-256 values are
in the respective scaling metadata files.

Each of five 500 ms repetitions alternated v2 then v3. Arguments were
`--mode buffered --read-percent 0 --distribution uniform`, with one pinned
worker on CPU 0 or 12 pinned workers on CPUs
`0,6,1,7,2,8,3,9,4,10,5,11`. The [raw CSV](raw.csv) records every run.

| Workers | v2 SET/s | Optimized v3 SET/s | Throughput change | v2 / v3 write p99 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 730,456 | 789,845 | +8.1% | 3.11 / 3.02 µs |
| 12 | 468,169 | 422,027 | -9.9% | 117.97 / 143.63 µs |

Frame allocation and the lookup-table CRC32 recover much of the initial v3
regression, but the required sequence-and-checksum finalization still runs
under one WAL writer lock. The remaining 12-writer throughput and tail gap is
consistent with that serialized work. These runs are a controlled same-host
comparison, not a claim that the two formats provide identical recovery
metadata.
