# Contributing

The most valuable contribution to a benchmark written by one framework's maintainer is a better
implementation for another framework. The rule is written down in advance, in
[docs/CHALLENGE.md](docs/CHALLENGE.md): a submitted implementation that is correct, idiomatic and
faster replaces the one here and the tables are regenerated, including when that makes qb lose.
Read that page first; it also says what will not be accepted, and why.

## Before you send anything

- **Build it**: the recipe is in [README.md](README.md) (*Running it*). Every framework compiles
  from source inside this project, under one `CMAKE_CXX_FLAGS_RELEASE`; `QVO_QB_DIR` must name a qb
  source tree, and the configure step refuses to continue without one rather than measure nothing.
- **Verify it**: `tools/run.py` asserts every cell (the checksum of `benchmarks/specs/`, the message
  count, the pin read back, the measurement window) and writes the verdict into the document. A
  cell that did not verify is not a result.
- **Keep the documents honest**: `python tools/check-report.py` (every figure in `README.md` and
  `REPORT.md` against the JSON it came from) and `python tools/check-roster.py` (every cell the
  roster expects exists or is declared `n/a` with a reason) must both print `OK`. Their negative
  controls (`tools/guards-negative-control.py`, `tools/negative-control.py`) plant defects and prove
  the guards catch them; run them when you touch a guard.

## What a pull request carries

- One `.cpp` per (framework, benchmark) under `frameworks/<framework>/<suite>/`, with the header
  block every implementation carries (`@benchmark`, `@framework`, `@idiom-source`, `@idiom-note`).
- If you re-measured: the result documents under `results/<host-id>/`, a host `README.md` that
  describes the machine, the compiler, the pinning and the session, and a `REPORT.md` regenerated
  by `tools/report.py`. Never a table typed by hand.
- A commit message that says what changed and why. No attribution trailers or generated-with
  bylines of any kind.

## What an issue carries

Which number you do not believe, on which host, and why. "Your CAF implementation should have used
X" is a useful issue even without a patch — it is how the CAF tuning defect in
[docs/TUNING.md](docs/TUNING.md) §1 was found.
