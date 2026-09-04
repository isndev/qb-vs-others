# CAF adapter, detached variant: the frameworks/caf sources rebuilt with QVO_CAF_DETACHED, which
# spawns every actor `caf::detached` (one private OS thread per actor, pinned through
# caf::thread_hook). ONLY ping-pong is mirrored (savina/ping-pong.cpp is the one file in this
# directory's suite): the variant exists to give CAF a two-actor cross-core cell, and on the
# many-actor benchmarks (100 ring actors, 60 workers, 120 all-to-all) a thread per actor on two
# CPUs would measure the OS scheduler, not CAF -- README.md says why in full. The line below is
# what tools/check-roster.py reads; an omission it names that stops being a spec is an error.
# qvo-roster-omit: savina/counting savina/thread-ring savina/fork-join savina/big -- a thread per actor on two CPUs measures the OS scheduler, not CAF; ping-pong is the only two-actor shape
qvo_add_all_benchmarks(FRAMEWORK caf-detached SUITES savina LIBS CAF::core DEFINES QVO_CAF_DETACHED=1)
