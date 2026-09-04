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

set(QVO_QB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../qb" CACHE PATH
    "Path to the qb source tree (default: the sibling submodule in qb-dev)")

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

qvo_report_configuration()
