# Third-party notices

Nothing from another project is vendored in this repository. The frameworks under test are fetched
from their own repositories at configure time, at the pinned refs `cmake/qvoFrameworks.cmake`
records, built from source under this project's flags, and never redistributed by it.

| Component | Version | Obtained | License |
|---|---|---|---|
| [qb](https://github.com/isndev/qb) | the version in every result document's `framework_version` | a source tree named by `QVO_QB_DIR` | Apache-2.0 |
| [CAF](https://github.com/actor-framework/actor-framework) (C++ Actor Framework) | 1.1.0 | `FetchContent`, tag `1.1.0` | BSD-3-Clause |
| [SObjectizer](https://github.com/stiffstream/sobjectizer) | 5.8.5.1 | `FetchContent`, tag `v5.8.5.1` | BSD-3-Clause |
| [Savina](https://github.com/shamsimam/savina) | — | not used: the *shapes* are re-implemented per framework under `frameworks/`, with the checksums of `benchmarks/specs/` as the correctness oracle | the paper: S. Imam and V. Sarkar, *Savina — An Actor Benchmark Suite*, AGERE 2014 |
| [nlohmann/json](https://github.com/nlohmann/json), [Google Benchmark](https://github.com/google/benchmark), [OpenSSL](https://www.openssl.org/), [zlib](https://zlib.net/) | as pinned by `vcpkg.json` | vcpkg | MIT, Apache-2.0, Apache-2.0, zlib |

The harness, the adapters, the specs, the tools and the result documents are this repository's own
work, under [LICENSE](LICENSE) (Apache-2.0). The result documents are data: reuse them with the
attribution the license asks for, and with the host README they were measured under, because a
figure without its protocol is not a measurement.
