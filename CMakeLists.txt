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
# The selected configuration's common flags are recorded in `env.cxx_flags`. Target-specific
# options remain separate; the result records this scope explicitly.
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

# Each configuration receives its own literal header; generated after all framework declarations.
# This changes metadata only, never the optimization/debug flags above or framework target options.
target_include_directories(qvo-harness PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/qvo-build-metadata/$<CONFIG>")

# Independent of the timing harness and qb: parser controls do not create an actor engine.
add_library(qvo-probe-memory STATIC EXCLUDE_FROM_ALL harness/src/process-memory.cpp)
target_include_directories(qvo-probe-memory PUBLIC harness/include)
if(WIN32)
    target_link_libraries(qvo-probe-memory PUBLIC psapi)
endif()

if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(qvo-harness PUBLIC Threads::Threads)
endif()
if(WIN32)
    target_link_libraries(qvo-harness PUBLIC shell32)  # CommandLineToArgvW for Unicode --out paths
endif()

# -------------------------------------------------------------------------------------------
# Adapters
# -------------------------------------------------------------------------------------------

qvo_declare_frameworks()

foreach(_fw IN LISTS QVO_ENABLED_FRAMEWORKS)
    add_subdirectory(frameworks/${_fw})
endforeach()

# -------------------------------------------------------------------------------------------
# The negative control's subjects
# -------------------------------------------------------------------------------------------
#
# Named `qvoctl-` and NOT declared through qvo_add_benchmark, so tools/run.py -- which discovers
# binaries whose name begins with `qvo-` -- cannot find them. A planted defect must never be able
# to reach a published table. The ping-pong subject plants defects in the harness's verification,
# the thread-ring one in savina/thread-ring's own (Huly QB-1017), and the twelve B/C subjects
# in their own shape's verifier (Huly QB-246, QB-247).
option(QVO_BUILD_CONTROLS "Build the negative-control subjects" ON)
if(QVO_BUILD_CONTROLS)
    foreach(_subject control-subject metadata-subject thread-ring-subject radixsort-subject filterbank-subject
                     sieve-subject trapezoid-subject piprecision-subject recmatmul-subject
                     quicksort-subject bitonicsort-subject apsp-subject sor-subject
                     uct-subject facloc-subject mesh-dispatch-subject)
        add_executable(qvoctl-${_subject} tools/control/${_subject}.cpp)
        target_link_libraries(qvoctl-${_subject} PRIVATE qvo-harness)
        set_target_properties(qvoctl-${_subject} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            FOLDER "controls")
    endforeach()

    add_executable(qvoctl-process-memory tools/control/process-memory-subject.cpp)
    target_link_libraries(qvoctl-process-memory PRIVATE qvo-probe-memory)
    set_target_properties(qvoctl-process-memory PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "controls")

    if("qb" IN_LIST QVO_ENABLED_FRAMEWORKS)
        add_executable(qvoctl-qb-caveats tools/control/qb-caveats-subject.cpp)
        target_link_libraries(qvoctl-qb-caveats PRIVATE qvo-harness qb::core)
        target_compile_definitions(qvoctl-qb-caveats PRIVATE
            QVO_FRAMEWORK_VERSION="${QVO_QB_VERSION}")
        set_target_properties(qvoctl-qb-caveats PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            FOLDER "controls")
    endif()

    add_executable(qvoctl-mesh-dispatch-reentrant tools/control/mesh-dispatch-subject.cpp)
    target_link_libraries(qvoctl-mesh-dispatch-reentrant PRIVATE qvo-harness)
    target_compile_definitions(qvoctl-mesh-dispatch-reentrant PRIVATE QVO_MESH_REENTRANT_CONTROL=1)
    set_target_properties(qvoctl-mesh-dispatch-reentrant PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "controls")

    # Real adapters with compile-only plants. Their qvoctl- names keep them out of discovery;
    # the ordinary qvo-* targets contain none of the plant state, branches or diagnostics.
    foreach(_fw qb baseline)
        if(NOT _fw IN_LIST QVO_ENABLED_FRAMEWORKS)
            continue()
        endif()
        string(TOUPPER "${_fw}" _FW)
        foreach(_shape apsp sor)
            set(_target "qvoctl-${_fw}-${_shape}")
            add_executable(${_target} "frameworks/${_fw}/savina/${_shape}.cpp")
            target_link_libraries(${_target} PRIVATE qvo-harness)
            if(_fw STREQUAL "qb")
                target_link_libraries(${_target} PRIVATE qb::core)
            endif()
            target_compile_definitions(${_target} PRIVATE
                QVO_REAL_ADAPTER_CONTROLS=1
                QVO_BENCHMARK_ID="savina/${_shape}"
                QVO_FRAMEWORK_ID="${_fw}"
                QVO_FRAMEWORK_VERSION="${QVO_${_FW}_VERSION}")
            set_target_properties(${_target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
                FOLDER "controls")
            if(WIN32)
                add_custom_command(TARGET ${_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${_target}>>,copy_if_different,true>
                            $<TARGET_RUNTIME_DLLS:${_target}> $<TARGET_FILE_DIR:${_target}>
                    COMMAND_EXPAND_LISTS)
            endif()
        endforeach()
    endforeach()
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
    target_link_libraries(qvoprobe-pipe-footprint PRIVATE qb::core qvo-probe-memory)
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


# Capture the final directory-wide flags once configuration has declared every dependency.
# Literal headers avoid command-line quoting and keep multi-config metadata tied to the binary.
function(_qvo_cxx_string output value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()

set(_qvo_metadata_configs "${CMAKE_CONFIGURATION_TYPES}")
if(NOT _qvo_metadata_configs)
    set(_qvo_metadata_configs "${CMAKE_BUILD_TYPE}")
endif()
foreach(_config IN LISTS _qvo_metadata_configs)
    string(TOUPPER "${_config}" _config_upper)
    set(_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_config_upper}}")
    string(STRIP "${_flags}" _flags)
    _qvo_cxx_string(_compiler "${CMAKE_CXX_COMPILER_ID}")
    _qvo_cxx_string(_compiler_version "${CMAKE_CXX_COMPILER_VERSION}")
    _qvo_cxx_string(_build_type "${_config}")
    _qvo_cxx_string(_cxx_flags "${_flags}")
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/qvo-build-metadata/${_config}/qvo")
    file(MAKE_DIRECTORY "${_dir}")
    string(CONCAT _header
        "#ifndef QVO_BUILD_INFO_H\n#define QVO_BUILD_INFO_H\n"
        "#define QVO_COMPILER ${_compiler}\n"
        "#define QVO_COMPILER_VERSION ${_compiler_version}\n"
        "#define QVO_BUILD_TYPE ${_build_type}\n"
        "#define QVO_CXX_FLAGS ${_cxx_flags}\n"
        "#define QVO_CXX_FLAGS_SCOPE \"directory-global-and-config\"\n"
        "#endif\n")
    set(_previous "")
    if(EXISTS "${_dir}/build-info.h")
        file(READ "${_dir}/build-info.h" _previous)
    endif()
    if(NOT "${_previous}" STREQUAL "${_header}")
        file(WRITE "${_dir}/build-info.h" "${_header}")
    endif()
endforeach()

qvo_report_configuration()
