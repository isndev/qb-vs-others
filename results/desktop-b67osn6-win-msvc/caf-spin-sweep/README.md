# caf-spin-sweep — CAF work-stealing knobs, swept

**A side experiment, not a table.** `tools/report.py` leaves this directory out (its documents
declare `savina/ping-pong`, which this directory is not named for) and says so on stderr. The
reading is in `docs/TUNING.md` §1.1; this directory is the evidence for it.

Windows 11 / MSVC 19.51, i9-12900K, quiet host, `savina/ping-pong` `cores=2 wait=1`, pinned to
CPUs 0 and 2, 5 repetitions + 2 warmup, 1 000 000 round trips each, 2026-09-04 13:47–13:48 UTC.
Binary: `build/final/bin/qvo-caf-savina-ping-pong.exe` (CAF 1.1.0). Driver: `caf-sweep.sh`,
which sets `QVO_CAF_AGGRESSIVE_POLL` / `QVO_CAF_STEAL_INTERVAL` and runs the same harness
command ten times; `sweep.log` is its stdout, one line per run (p50 / min / max per round trip).

| file | `aggressive-poll-attempts` | `aggressive-steal-interval` | ns / round trip, p50 [min, max] |
|---|---:|---:|---:|
| `poll100-steal10.json` | 100 (default) | 10 (default) | 493.4 [487, 529] |
| `poll1e3-steal10.json` | 1 000 | 10 | 533.8 [503, 544] |
| `poll1e4-steal10.json` | 10 000 | 10 | 718.8 [640, 781] |
| `poll1e5-steal10.json` | 100 000 | 10 | 730.0 [718, 853] |
| `poll1e6-steal10.json` | 1 000 000 | 10 | 752.1 [732, 894] |
| `poll1e4-steal1.json` | 10 000 | 1 | 754.4 [704, 1072] |
| `poll1e4-steal100.json` | 10 000 | 100 | 534.2 [516, 540] |
| `poll1e4-steal1e3.json` | 10 000 | 1 000 | 490.4 [488, 496] |
| `poll1e6-steal1e6.json` | 1 000 000 | 1 000 000 | 495.3 [492, 529] |
| `poll100-steal10-b.json` | 100 (default), re-run last | 10 (default) | 495.0 [490, 498] |

Two things the documents themselves do not say, because they predate the adapter change that
records them: the `caveats` array in these files is the pre-sweep wording ("the profile that
measured fastest"), and the override values are NOT in the JSON — they are in the file name and
in `sweep.log`. Since this sweep, `frameworks/caf/caf_support.h` writes a `SWEEP DOCUMENT, NOT A
TABLE CELL` caveat carrying both values into any document measured under an override, so a
future sweep file is self-describing. These ten are kept as measured; the file-name key is the
only provenance they carry, and this README is where it is written down.
