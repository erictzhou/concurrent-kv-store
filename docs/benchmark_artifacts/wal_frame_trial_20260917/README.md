# WAL frame-preparation trial

Five 500 ms runs each for Buffered and GroupCommit uniform SET with 1 and 12
pinned workers on `agency-bench`. The trial source contained the CRC32 table
and prepared-frame change later committed as `d040d98`. It was built with GCC
13.3.0 Release and passed the Dockerfile CTest gate. The [raw rows](raw.csv)
supported keeping the change: median Buffered SET increased to 788,680/s at
one worker and 415,985/s at 12; GroupCommit stayed near 2.45k and 25.75k/s.
The publication suite was then rerun from the committed Git archive, so the
main benchmark numbers come from that exact source commit instead of this
trial.
