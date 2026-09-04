# SObjectizer adapter. The static build exports `sobjectizer::StaticLib`.
qvo_add_all_benchmarks(FRAMEWORK sobjectizer SUITES savina micro LIBS sobjectizer::StaticLib)
