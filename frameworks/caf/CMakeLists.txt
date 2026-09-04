# CAF adapter. Only caf::core is exercised -- every benchmark here is in-process message passing.
qvo_add_all_benchmarks(FRAMEWORK caf SUITES savina micro LIBS CAF::core)
