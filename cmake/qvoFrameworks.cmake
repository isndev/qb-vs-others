#
# qvoFrameworks.cmake — acquires every framework under test, from source, at a pinned ref.
#
# Two rules govern this file, and both come from FAIRNESS.md:
#
#  1. Every framework is built from source inside THIS project, so `CMAKE_CXX_FLAGS_<CONFIG>`
#     applies to all of them identically. Nothing here consumes a prebuilt binary.
#  2. Every framework's ref is PINNED and recorded. `QVO_<FW>_VERSION` ends up verbatim in every
#     result file, so a table can never be compared against a version it was not measured on.
#
# The refs below are the same ones vcpkg's ports resolve at qb-dev's own pinned vcpkg baseline
# (a900048467…). That is deliberate: it means the versions compared here are the versions a user
# following qb-dev's dependency pinning would actually get.
#

include(FetchContent)

set(QVO_CAF_REF          "1.1.0"     CACHE STRING "CAF git tag")
set(QVO_SOBJECTIZER_REF  "v5.8.5.1"  CACHE STRING "SObjectizer git tag")

# An offline mirror: point these at an already-cloned tree to build with no network.
set(QVO_CAF_SOURCE_DIR         "" CACHE PATH "Pre-fetched CAF source tree")
set(QVO_SOBJECTIZER_SOURCE_DIR "" CACHE PATH "Pre-fetched SObjectizer source tree")

set(QVO_ENABLED_FRAMEWORKS "" CACHE INTERNAL "" FORCE)
set(QVO_SKIPPED_FRAMEWORKS "" CACHE INTERNAL "" FORCE)

function(_qvo_enable name)
    set(_l ${QVO_ENABLED_FRAMEWORKS})
    list(APPEND _l ${name})
    set(QVO_ENABLED_FRAMEWORKS ${_l} CACHE INTERNAL "" FORCE)
endfunction()

function(_qvo_skip name reason)
    set(_l ${QVO_SKIPPED_FRAMEWORKS})
    list(APPEND _l "${name}: ${reason}")
    set(QVO_SKIPPED_FRAMEWORKS ${_l} CACHE INTERNAL "" FORCE)
endfunction()

# -------------------------------------------------------------------------------------------

macro(_qvo_declare_qb)
    if(NOT EXISTS "${QVO_QB_DIR}/CMakeLists.txt")
        _qvo_skip(qb "QVO_QB_DIR=${QVO_QB_DIR} has no CMakeLists.txt")
    else()
        # A benchmark of the actor engine needs neither TLS, nor compression, nor qb's own tests.
        # Turning them off removes dependencies the other frameworks do not have, which keeps the
        # comparison about the actor runtime rather than about what each library happens to bundle.
        set(QB_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
        set(QB_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
        set(QB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(QB_INSTALL          OFF CACHE BOOL "" FORCE)
        set(QB_WITH_SSL         OFF CACHE BOOL "" FORCE)
        set(QB_WITH_COMPRESSION OFF CACHE BOOL "" FORCE)
        set(QB_ENABLE_NATIVE_ARCH ${QVO_NATIVE_ARCH} CACHE BOOL "" FORCE)

        add_subdirectory("${QVO_QB_DIR}" "${CMAKE_BINARY_DIR}/_deps/qb-build" EXCLUDE_FROM_ALL)

        # The version source of truth is qb's own cmake config, never a literal typed here.
        if(DEFINED QB_FRAMEWORK_VERSION)
            set(QVO_QB_VERSION "${QB_FRAMEWORK_VERSION}" CACHE INTERNAL "" FORCE)
        else()
            set(QVO_QB_VERSION "unknown" CACHE INTERNAL "" FORCE)
        endif()

        _qvo_enable(qb)
    endif()
endmacro()

macro(_qvo_declare_caf)
    set(CAF_ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
    set(CAF_ENABLE_EXAMPLES       OFF CACHE BOOL "" FORCE)
    set(CAF_ENABLE_TOOLS          OFF CACHE BOOL "" FORCE)
    # Only caf::core is exercised: every benchmark here is in-process message passing. Building
    # the io/net modules would add code none of the measured paths touch.
    set(CAF_ENABLE_IO_MODULE      OFF CACHE BOOL "" FORCE)
    set(CAF_ENABLE_NET_MODULE     OFF CACHE BOOL "" FORCE)
    # OFF, matching vcpkg's own port. Runtime checks are a debugging aid; leaving them on would
    # measure CAF's assertions rather than CAF.
    set(CAF_ENABLE_RUNTIME_CHECKS OFF CACHE BOOL "" FORCE)
    set(CAF_ENABLE_ACTOR_PROFILER OFF CACHE BOOL "" FORCE)
    set(CAF_ENABLE_EXCEPTIONS     ON  CACHE BOOL "" FORCE)

    if(QVO_CAF_SOURCE_DIR)
        FetchContent_Declare(caf SOURCE_DIR "${QVO_CAF_SOURCE_DIR}" DOWNLOAD_COMMAND "")
    else()
        FetchContent_Declare(caf
            GIT_REPOSITORY https://github.com/actor-framework/actor-framework.git
            GIT_TAG        ${QVO_CAF_REF}
            GIT_SHALLOW    TRUE)
    endif()
    FetchContent_MakeAvailable(caf)
    set(QVO_CAF_VERSION "${QVO_CAF_REF}" CACHE INTERNAL "" FORCE)
    _qvo_enable(caf)
endmacro()

macro(_qvo_declare_sobjectizer)
    set(SOBJECTIZER_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    set(SOBJECTIZER_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(SOBJECTIZER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(SOBJECTIZER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

    if(QVO_SOBJECTIZER_SOURCE_DIR)
        FetchContent_Declare(sobjectizer SOURCE_DIR "${QVO_SOBJECTIZER_SOURCE_DIR}"
                             DOWNLOAD_COMMAND "" SOURCE_SUBDIR dev)
    else()
        FetchContent_Declare(sobjectizer
            GIT_REPOSITORY https://github.com/stiffstream/sobjectizer.git
            GIT_TAG        ${QVO_SOBJECTIZER_REF}
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  dev)
    endif()
    FetchContent_MakeAvailable(sobjectizer)
    string(REGEX REPLACE "^v" "" _so_ver "${QVO_SOBJECTIZER_REF}")
    set(QVO_SOBJECTIZER_VERSION "${_so_ver}" CACHE INTERNAL "" FORCE)
    _qvo_enable(sobjectizer)
endmacro()

macro(_qvo_declare_baseline)
    # No acquisition: the floor is written in this repository, in frameworks/baseline/.
    set(QVO_BASELINE_VERSION "in-tree" CACHE INTERNAL "" FORCE)
    _qvo_enable(baseline)
endmacro()

macro(_qvo_declare_seastar)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        _qvo_skip(seastar "Linux only -- it has no Windows or macOS port")
    else()
        find_package(Seastar QUIET)
        if(NOT Seastar_FOUND)
            _qvo_skip(seastar "find_package(Seastar) failed; see docs/SEASTAR.md")
        else()
            set(QVO_SEASTAR_VERSION "${Seastar_VERSION}" CACHE INTERNAL "" FORCE)
            _qvo_enable(seastar)
        endif()
    endif()
endmacro()

# -------------------------------------------------------------------------------------------

macro(qvo_declare_frameworks)
    if(QVO_WITH_BASELINE)
        _qvo_declare_baseline()
    endif()
    if(QVO_WITH_QB)
        _qvo_declare_qb()
    endif()
    if(QVO_WITH_CAF)
        _qvo_declare_caf()
    endif()
    if(QVO_WITH_SOBJECTIZER)
        _qvo_declare_sobjectizer()
    endif()
    if(QVO_WITH_SEASTAR)
        _qvo_declare_seastar()
    endif()
endmacro()

function(qvo_report_configuration)
    message(STATUS "")
    message(STATUS "  qb-vs-others configuration")
    message(STATUS "  --------------------------")
    message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
    message(STATUS "  opt flags  : ${QVO_OPT_FLAGS}")
    foreach(_fw IN LISTS QVO_ENABLED_FRAMEWORKS)
        string(TOUPPER ${_fw} _FW)
        message(STATUS "  framework  : ${_fw} ${QVO_${_FW}_VERSION}")
    endforeach()
    foreach(_s IN LISTS QVO_SKIPPED_FRAMEWORKS)
        message(STATUS "  SKIPPED    : ${_s}")
    endforeach()
    message(STATUS "")
endfunction()
