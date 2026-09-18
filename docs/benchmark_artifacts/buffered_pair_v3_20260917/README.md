# Buffered WAL v2/v3 paired check

`agency-bench`, AMD Ryzen 9 5900X, home-directory ext4. Both binaries were
built with GCC 13.3.0 in Release mode. The v2 binary came from
`596a72eb9c4df7ae037851acf30af471f48369c3`; the initial v3 binary came
from `12fb33e184f7f85a020c1c30408a442a28cf1f30`. Their SHA-256 values
are in the corresponding scaling metadata files. These are independent WAL
files, removed after each invocation.

Each repetition alternated v2 then v3 at a fixed worker count, for five
500 ms repetitions each. The command arguments were `--mode buffered
--read-percent 0 --distribution uniform --duration-ms 500`, with either
`--threads 1 --cpus 0` or `--threads 12 --cpus
0,6,1,7,2,8,3,9,4,10,5,11`. The [raw CSV](raw.csv) includes throughput and
write p99 for every invocation.

| Workers | v2 median SET/s | Initial v3 median SET/s | v3 change |
| ---: | ---: | ---: | ---: |
| 1 | 727,676 | 660,718 | -9.2% |
| 12 | 461,195 | 362,448 | -21.4% |

The `v2` and `v3` perf stat files and sampled call-stack reports are one-second
12-worker profiles, separate from the five timed repetitions. The v3 report
shows `WriteAheadLog::EncodeFrameLocked` among sampled CPU hot spots. Moving
frame allocation outside the WAL I/O lock and using a lookup-table CRC32 were
then tested in a later commit; the final optimized results are reported in
the main benchmark document.
