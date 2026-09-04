# qb adapter. Links qb::core (which brings qb::io transitively).
qvo_add_all_benchmarks(FRAMEWORK qb SUITES savina micro LIBS qb::core)
