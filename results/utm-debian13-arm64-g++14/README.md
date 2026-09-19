# utm-debian13-arm64-g++14

A **native-arm64 Linux guest** — UTM / QEMU with hardware virtualisation (`systemd-detect-virt`:
`qemu`) on the Apple M4 Pro that is also `../macbook-m4pro-macos-clang21/` — Debian 13.7, kernel
6.12.107+deb13-arm64, 10 vCPUs, 11 GB, g++ 14.2.0, `-O3 -DNDEBUG`, pinned to vCPUs 2 and 4,
**9 repetitions + 2 warmup**. Every one of the **132 cells** across the eight `savina-*/`
directories was measured in one quiet session on **2026-09-19, 13:30:28–13:45:59 UTC** — 130
verified + 2 declared `n/a` (`caf-detached` has no spin mode), one build, one manifest. **qb in
`savina-*/` is the 3.2.0 candidate, `develop` `174e515a`**, as in the macOS directory measured an
hour earlier; shipped v3.1.0 (`830ea244`) was measured minutes before the field in the same session
(`qb-branch-develop/grid-shipped-3.1.0-20260919/`). CAF 1.1.0, SObjectizer 5.8.5.1 and the
raw-thread floor come from the same build (`~/qvo/linux`). `tools/check-roster.py --results
results/utm-debian13-arm64-g++14` is clean.

This is the repository's first Linux host that is not WSL2, and its first Linux on arm64 — what
`docs/TUNING.md` §7 listed as still open ("native Linux", "arm64") as far as a guest can close it.
**It is still a guest**: the pin is verified by Linux and it pins a vCPU, the host schedules the
vCPU threads, and a futex wake across two vCPUs costs the hypervisor — `baseline__2c-park` reads
**20.8 µs per ping-pong round trip and 10.6 µs per ring hop**, WSL2's caveat word for word (24.8 and
12.6 in its current field): every `2c-park` cell of a shape that crosses a core per message measures that wake for
any framework that truly parks — shipped qb 3.1.0 29.6 µs, SObjectizer 22.1 µs, `caf-detached`
21.8 µs — and the candidate's 0.17 µs is the 50 µs idle-spin floor never letting the pair sleep. The pooled `caf` row (0.30 µs) is below the floor because it never crosses a core.

`REPORT.md` beside this file is `tools/report.py`'s render of this directory and
`tools/check-report.py` fails if it drifts. `docs/TUNING.md` §13.9 is the reading guide;
`qb-branch-develop/README.md` carries the session's protocol, its controls, its censuses and the one
cell this host reads differently from the three others.

| directory | what it is |
|---|---|
| `savina-ping-pong/` | **20 cells** — 18 verified + 2 declared `n/a` (`caf-detached` has no spin mode). |
| `savina-counting/`, `savina-thread-ring/`, `savina-fork-join/`, `savina-big/`, `savina-fib/`, `savina-chameneos/`, `savina-bank-transaction/` | **16 cells** each, all verified; `caf-detached` declares itself omitted from these seven. |
| `qb-branch-develop/` | **the 3.2.0 candidate on this host**: `grid-174e515a/` and its second pass, the two controls (`grid-shipped-3.1.0-20260919/`, `grid-f2779605-20260919/`), the two censuses and `bisect-f2779605-174e515a/`. |
