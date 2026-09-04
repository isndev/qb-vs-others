#
# qvoBenchmark.cmake — declares one benchmark binary for one framework.
#
# The target name and the benchmark identity are DERIVED from the source path, never typed:
#
#     frameworks/caf/savina/thread-ring.cpp
#         -> target    qvo-caf-savina-thread-ring
#         -> benchmark "savina/thread-ring"
#         -> framework "caf"
#
# There is therefore nowhere to type a name that disagrees with the file under it, which is the
# same rule qb's own example tree enforces and for the same reason: a comparison table keyed on a
# hand-written label is one rename away from attributing a measurement to the wrong framework.
#
# The `benchmark` string that reaches the result JSON comes from `QVO_BENCHMARK_ID`, a compile
# definition set here. An implementation that hard-codes a different one fails the consistency
# check in tools/check-results.py.
#

set(QVO_ALL_BENCHMARK_TARGETS "" CACHE INTERNAL "" FORCE)

function(qvo_add_benchmark)
    cmake_parse_arguments(A "" "SOURCE;FRAMEWORK" "LIBS;DEFINES" ${ARGN})

    if(NOT A_SOURCE)
        message(FATAL_ERROR "qvo_add_benchmark: SOURCE is required")
    endif()
    if(NOT A_FRAMEWORK)
        message(FATAL_ERROR "qvo_add_benchmark: FRAMEWORK is required")
    endif()

    # <suite>/<slug> from the path, with the same NN- tolerance qb's derivation has.
    get_filename_component(_dir  "${A_SOURCE}" DIRECTORY)
    get_filename_component(_slug "${A_SOURCE}" NAME_WE)
    get_filename_component(_suite "${_dir}" NAME)

    if(NOT _slug MATCHES "^[a-z0-9]+(-[a-z0-9]+)*$")
        message(FATAL_ERROR
            "qvo_add_benchmark: '${A_SOURCE}' -- a benchmark file name must be a lowercase "
            "hyphenated slug (got '${_slug}'). The target, the binary and the benchmark id are "
            "all derived from it.")
    endif()
    if(NOT _suite MATCHES "^[a-z0-9]+(-[a-z0-9]+)*$")
        message(FATAL_ERROR
            "qvo_add_benchmark: '${A_SOURCE}' -- the suite directory must be a lowercase "
            "hyphenated slug (got '${_suite}').")
    endif()

    set(_bench_id "${_suite}/${_slug}")
    set(_target   "qvo-${A_FRAMEWORK}-${_suite}-${_slug}")

    add_executable(${_target} "${A_SOURCE}")
    target_link_libraries(${_target} PRIVATE qvo-harness ${A_LIBS})

    # `caf-detached` -> QVO_CAF_DETACHED_VERSION: a hyphen is legal in a CMake variable name but
    # not in the macro a framework's version is spelled through, so it is normalised here once.
    string(TOUPPER ${A_FRAMEWORK} _FW)
    string(REPLACE "-" "_" _FW "${_FW}")
    if(NOT DEFINED QVO_${_FW}_VERSION)
        message(FATAL_ERROR
            "qvo_add_benchmark: framework '${A_FRAMEWORK}' has no QVO_${_FW}_VERSION -- declare "
            "it in cmake/qvoFrameworks.cmake; a result JSON without a version is not attributable.")
    endif()
    target_compile_definitions(${_target} PRIVATE
        QVO_BENCHMARK_ID="${_bench_id}"
        QVO_FRAMEWORK_ID="${A_FRAMEWORK}"
        QVO_FRAMEWORK_VERSION="${QVO_${_FW}_VERSION}"
        ${A_DEFINES})

    set_target_properties(${_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        # No OUTPUT_NAME: the binary is the target, so the roster, the target and the file on disk
        # cannot drift apart.
        FOLDER "benchmarks/${A_FRAMEWORK}")

    # Windows: vcpkg Find-module dependencies resolve as UNKNOWN_LIBRARY, for which
    # $<TARGET_RUNTIME_DLLS> is empty -- qb's own history records a whole example corpus that
    # built cleanly and could not load a single DLL because of exactly this. Copy what CMake does
    # know about, and let tools/run.py report a load failure as a load failure.
    if(WIN32)
        add_custom_command(TARGET ${_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${_target}>>,copy_if_different,true>
                    $<TARGET_RUNTIME_DLLS:${_target}> $<TARGET_FILE_DIR:${_target}>
            COMMAND_EXPAND_LISTS)
    endif()

    set(_l ${QVO_ALL_BENCHMARK_TARGETS})
    list(APPEND _l "${_target}")
    set(QVO_ALL_BENCHMARK_TARGETS ${_l} CACHE INTERNAL "" FORCE)
endfunction()

# Declares every `<suite>/*.cpp` under the calling framework directory. A benchmark added to the
# tree is in scope on the same commit, with no list to remember -- and a benchmark that vanishes
# is caught by tools/check-results.py, which cross-checks the built roster against the spec set in
# BOTH directions.
function(qvo_add_all_benchmarks)
    cmake_parse_arguments(A "" "FRAMEWORK" "SUITES;LIBS;DEFINES" ${ARGN})
    foreach(_suite IN LISTS A_SUITES)
        file(GLOB _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_suite}/*.cpp")
        foreach(_src IN LISTS _srcs)
            qvo_add_benchmark(SOURCE "${_src}" FRAMEWORK "${A_FRAMEWORK}" LIBS ${A_LIBS}
                              DEFINES ${A_DEFINES})
        endforeach()
    endforeach()
endfunction()
