#
# qb-vs-others — cross-framework actor benchmark harness
#
# ONE flag set, applied to every framework and every benchmark binary in this project.
#
# This is not a stylistic preference: it is FAIRNESS.md mechanism 1.3, and it is the reason the
# frameworks are built FROM SOURCE here instead of being consumed as prebuilt packages. A
# prebuilt CAF from a package manager is compiled with that package manager's release flags,
# while qb built in-tree would be compiled with ours. The difference between two MSVC release
# flag sets is small, but "small" is not "measured", and a comparison whose subjects were
# optimized differently cannot answer the question it claims to answer.
#
cmake_minimum_required(VERSION 3.25)

project(qb-vs-others
        VERSION 0.1.0
        DESCRIPTION "Fair, verified benchmark comparison of qb against other actor frameworks"
        LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Frameworks are third-party sources we do not own. Their warnings are not this repository's
# business and must not turn into an argument for building one of them differently.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)   # option() honours a normal variable set beforehand
set(CMAKE_POLICY_DEFAULT_CMP0079 NEW)

# -------------------------------------------------------------------------------------------
# The single optimization contract
# -------------------------------------------------------------------------------------------
#
# Recorded verbatim into every result file as `env.cxx_flags`, so a reader never has to trust
# this comment.
#
if(MSVC)
    # /Ob2 is forced rather than inherited: MSVC's RelWithDebInfo and MinSizeRel defaults carry
    # /Ob1, which breaks C++20 coroutine symmetric transfer (C4737). qb's own build already
    # forces it for that reason; applying it here uniformly keeps coroutine-using and
    # non-coroutine-using frameworks on the same inlining contract.
    set(QVO_OPT_FLAGS "/O2 /Ob2 /DNDEBUG")
else()
    set(QVO_OPT_FLAGS "-O3 -DNDEBUG")
endif()

# Set as the CONFIG-wide flags rather than per-target, because per-target options would reach the
# benchmark binaries and NOT the framework libraries -- which is precisely the asymmetry this
# repository exists to avoid.
#
# What this guarantees, exactly: every framework is compiled at the same optimization level, from
# source, by the same compiler invocation prefix. What it does NOT claim: that the resulting
# command lines are byte-identical. Each framework's own CMake adds flags its maintainers chose
# (qb forces coroutine-safe inlining; CAF sets its own warning and visibility policy). Removing
# those would mean measuring a build none of these projects ships. They are part of the framework,
# and the honest contract is a common baseline plus each project's own definition on top of it.
set(CMAKE_CXX_FLAGS_RELEASE        "${QVO_OPT_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${QVO_OPT_FLAGS}" CACHE STRING "" FORCE)

# -march=native is deliberately OFF. It would let the compiler pick different instruction sets
# for translation units compiled at different times, and it makes a result file describe a
# binary nobody else can reproduce.
option(QVO_NATIVE_ARCH "Build with -march=native (NOT used for published results)" OFF)

# -------------------------------------------------------------------------------------------
# Where the frameworks come from
# -------------------------------------------------------------------------------------------

# qb is read from a source tree you name. The default is the sibling checkout this repository has
# inside its development superproject; a standalone clone passes -DQVO_QB_DIR=/path/to/qb, and a
# missing tree is a configure ERROR (cmake/qvoFrameworks.cmake), not a silent skip of the subject.
set(QVO_QB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../qb" CACHE PATH
    "Path to the qb source tree (the sibling checkout by default; set it in a standalone clone)")

option(QVO_WITH_QB          "Build the qb adapter"           ON)
option(QVO_WITH_CAF         "Build the CAF adapter"          ON)
option(QVO_WITH_SOBJECTIZER "Build the SObjectizer adapter"  ON)
option(QVO_WITH_BASELINE    "Build the raw-thread floor"     ON)
option(QVO_WITH_SEASTAR     "Build the Seastar adapter (Linux only)" OFF)

include(cmake/qvoFrameworks.cmake)
include(cmake/qvoBenchmark.cmake)

# -------------------------------------------------------------------------------------------
# The harness — the only timing/verification code in the repository
# -------------------------------------------------------------------------------------------

add_library(qvo-harness STATIC harness/src/harness.cpp)
target_include_directories(qvo-harness PUBLIC harness/include benchmarks/specs)

target_compile_definitions(qvo-harness PRIVATE
    QVO_COMPILER="${CMAKE_CXX_COMPILER_ID}"
    QVO_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    QVO_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    QVO_CXX_FLAGS="${QVO_OPT_FLAGS}")

if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(qvo-harness PUBLIC Threads::Threads)
endif()

# -------------------------------------------------------------------------------------------
# Adapters
# -------------------------------------------------------------------------------------------

qvo_declare_frameworks()

foreach(_fw IN LISTS QVO_ENABLED_FRAMEWORKS)
    add_subdirectory(frameworks/${_fw})
endforeach()

# -------------------------------------------------------------------------------------------
# The negative control's subject
# -------------------------------------------------------------------------------------------
#
# Named `qvoctl-` and NOT declared through qvo_add_benchmark, so tools/run.py -- which discovers
# binaries whose name begins with `qvo-` -- cannot find it. A planted defect must never be able to
# reach a published table.
option(QVO_BUILD_CONTROLS "Build the negative-control subject" ON)
if(QVO_BUILD_CONTROLS)
    add_executable(qvoctl-control-subject tools/control/control-subject.cpp)
    target_link_libraries(qvoctl-control-subject PRIVATE qvo-harness)
    set_target_properties(qvoctl-control-subject PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "controls")
endif()

# -------------------------------------------------------------------------------------------
# Probes: qb-only instruments behind docs/TUNING.md
# -------------------------------------------------------------------------------------------
#
# A probe measures one qb mechanism that has no counterpart in the other frameworks -- section 10's
# `parked-io-wake` asks what an idle actor core that OWNS io watchers pays to be woken by one of
# them, and neither CAF nor SObjectizer owns a socket. Named `qvoprobe-` and NOT declared through
# qvo_add_benchmark, for the same reason as the control subject: tools/run.py cannot discover it
# and a probe's figure can never be mistaken for a cross-framework cell.
option(QVO_BUILD_PROBES "Build the qb-only probes behind docs/TUNING.md" ON)
if(QVO_BUILD_PROBES AND "qb" IN_LIST QVO_ENABLED_FRAMEWORKS)
    add_executable(qvoprobe-parked-io-wake tools/probes/parked-io-wake.cpp)
    target_link_libraries(qvoprobe-parked-io-wake PRIVATE qb::core)
    set_target_properties(qvoprobe-parked-io-wake PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    add_executable(qvoprobe-pass-cost tools/probes/pass-cost.cpp)
    target_link_libraries(qvoprobe-pass-cost PRIVATE qb::core)
    set_target_properties(qvoprobe-pass-cost PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    add_executable(qvoprobe-xcore-hop tools/probes/xcore-hop.cpp)
    target_link_libraries(qvoprobe-xcore-hop PRIVATE qb::core)
    set_target_properties(qvoprobe-xcore-hop PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    add_executable(qvoprobe-ask-cost tools/probes/ask-cost.cpp)
    target_link_libraries(qvoprobe-ask-cost PRIVATE qb::core)
    set_target_properties(qvoprobe-ask-cost PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    # The parked timer wake (Huly QB-196): how late a parked core fires a timer it owns -- the
    # backend's wait granularity (whole milliseconds for epoll/wepoll, the system tick on Windows
    # unless the process raised its resolution, which the probe can do with timeBeginPeriod).
    add_executable(qvoprobe-parked-timer-wake tools/probes/parked-timer-wake.cpp)
    target_link_libraries(qvoprobe-parked-timer-wake PRIVATE qb::core)
    if(WIN32)
        target_link_libraries(qvoprobe-parked-timer-wake PRIVATE winmm)
    endif()
    set_target_properties(qvoprobe-parked-timer-wake PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    # The idle cadence (Huly QB-48): how often a core with nothing to do and `setLatency(L)` really
    # wakes, through its condition-variable park and through its loop park -- the wait's granularity
    # on each, per host.
    add_executable(qvoprobe-parked-cadence tools/probes/parked-cadence.cpp)
    target_link_libraries(qvoprobe-parked-cadence PRIVATE qb::core)
    set_target_properties(qvoprobe-parked-cadence PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    # The io pass (Huly QB-191): what a quiet socket costs a core per pass, and what a byte on it
    # waits for. The same source builds against a tree with or without the poll-interval knob.
    add_executable(qvoprobe-io-pass tools/probes/io-pass.cpp)
    target_link_libraries(qvoprobe-io-pass PRIVATE qb::core)
    set_target_properties(qvoprobe-io-pass PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    file(STRINGS "${QVO_QB_DIR}/src/qb/core/Main.h" _qvo_poll_knob REGEX "setIoPollInterval")
    if(_qvo_poll_knob)
        target_compile_definitions(qvoprobe-io-pass PRIVATE QVO_QB_HAS_IO_POLL_INTERVAL=1)
    endif()
    # The dispatch under a population (Huly QB-198): what one event costs a core serving N actors as
    # N grows past what its caches hold -- the shape the dispatcher's prefetch is for.
    add_executable(qvoprobe-dispatch-population tools/probes/dispatch-population.cpp)
    target_link_libraries(qvoprobe-dispatch-population PRIVATE qb::core)
    set_target_properties(qvoprobe-dispatch-population PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    # The footprint at rest (Huly QB-63): what an engine of N cores holds resident before any event
    # (the per-producer mailbox rings, value-initialised at start) and what the first traffic on
    # every pipe adds (one 256 KB segment per pipe used) -- the two quadratic terms, separated.
    add_executable(qvoprobe-pipe-footprint tools/probes/pipe-footprint.cpp)
    target_link_libraries(qvoprobe-pipe-footprint PRIVATE qb::core)
    set_target_properties(qvoprobe-pipe-footprint PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
    # No qb in it: the raw ring shapes behind docs/TUNING.md section 16. Linux / x86-64 only for real;
    # elsewhere the same source builds to a stub that says so and exits 2.
    add_executable(qvoprobe-raw-ring tools/probes/raw-ring.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(qvoprobe-raw-ring PRIVATE -mmovdir64b -mavx2)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(qvoprobe-raw-ring PRIVATE Threads::Threads)
    set_target_properties(qvoprobe-raw-ring PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "probes")
endif()

qvo_report_configuration()
