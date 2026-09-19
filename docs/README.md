# docs/

| Document | What it is |
|---|---|
| [../FAIRNESS.md](../FAIRNESS.md) | The protocol: the one fact the whole repository rests on, the seven mechanisms that keep a comparison fair, what it cannot tell you, how to reproduce it. Read first. |
| [CHALLENGE.md](CHALLENGE.md) | The right of reply: a correct, idiomatic, faster implementation replaces the one here and the tables are regenerated, including when qb loses. |
| [TUNING.md](TUNING.md) | The configuration sweeps and every qb-side finding, in the order they happened: the CAF profile that made CAF 2× slower, qb's parked mode, the 3.2.0 candidate grids on four hosts, and what still loses. Long; its table of contents is at the top. |
| [FEATURES.md](FEATURES.md) | What each framework offers, cited to its source. |
| [ROADMAP.md](ROADMAP.md) | What is not done: the other Savina shapes, Seastar, the cross-language references. |

The generated report is [../REPORT.md](../REPORT.md); every figure in it and in the README comes
from a document under `../results/` and `tools/check-report.py` fails if one does not.
