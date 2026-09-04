# CAF adapter, detached variant: the frameworks/caf sources rebuilt with QVO_CAF_DETACHED, which
# spawns every actor `caf::detached` (one private OS thread per actor, pinned through
# caf::thread_hook). Only the Savina suite is mirrored -- the micro suite measures the pool.
qvo_add_all_benchmarks(FRAMEWORK caf-detached SUITES savina LIBS CAF::core DEFINES QVO_CAF_DETACHED=1)
