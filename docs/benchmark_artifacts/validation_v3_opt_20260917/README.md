# Validation for the optimized WAL v3 benchmark binary

Source: `d040d98f22c0edfacae78958222e4b4c0e35d318`, staged on
`agency-bench` from `git archive`. Ubuntu 24.04, GCC 13.3.0, Release build
(`-O3 -DNDEBUG`). The scaling binary SHA-256 is recorded in
[`binary.sha256`](binary.sha256) and the scaling metadata.

- [`validation.log`](validation.log): Dockerfile Release build and CTest,
  102/102 passed before the timed sweep.
- [`tsan.log`](tsan.log): RelWithDebInfo ThreadSanitizer build and CTest,
  102/102 passed.
- [`asan.log`](asan.log): RelWithDebInfo AddressSanitizer/UBSan build and
  CTest, 102/102 passed with leak detection enabled.

The Linux TSan runtime needed `setarch x86_64 -R` in a container with
`--security-opt seccomp=unconfined` to initialize on this host. Its
simultaneously-held-lock tracker could not represent all 64 shard locks, so
`TSAN_OPTIONS=detect_deadlocks=0:halt_on_error=1` disabled that tracker;
data-race instrumentation remained enabled. The test suite includes recovery,
corruption, injected persistence faults, process exit, and high-contention
stress cases.
