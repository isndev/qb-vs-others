# wsl-debian-g++14

WSL2 (Hyper-V guest) — Debian 13, g++ 14.2.0, `-O3 -DNDEBUG`, i9-12900K, pinned to CPUs 0 and 2,
5 repetitions + 1 warmup, 2026-09-04. `docs/TUNING.md` §6 is the reading guide.

**Caveat that applies to every `2c-park` cell here:** a futex wake of a thread parked on another
vCPU costs ~13 µs under WSL2, so the raw condition-variable floor is 26.6 µs per round trip and any
framework that parks across cores measures the hypervisor, not itself. The `1c-*` and `2c-spin`
cells are unaffected.

The qb cells were measured against qb `develop` at `eac739ff` (v3.1.0), rebuilt clean for the
purpose; the instrumented `perf/mailbox-lost-wakeup` branch was NOT the binary under test.
