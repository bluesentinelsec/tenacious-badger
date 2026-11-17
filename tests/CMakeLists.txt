add_subdirectory(framework)

# Every test case of the project, in one executable: tb_tests. Add the source
# of a new test here. Suites are named <module>_<feature>, so one pattern
# selects a module (tb_tests "core_*") or a feature (tb_tests "core_sum.*").
set(TB_TEST_CASE_SOURCES
    core/allocator_test.c
    core/arena_test.c
    core/array_test.c
    core/assert_test.c
    core/atom_test.c
    core/bit_test.c
    core/fmt_test.c
    core/hash_test.c
    core/list_test.c
    core/memory_test.c
    core/ring_test.c
    core/set_test.c
    core/size_test.c
    core/str_test.c
    core/sum_test.c
    core/table_test.c
    core/text_test.c
    framework/selftest/checks_test.c
    framework/selftest/options_test.c
    framework/selftest/pattern_test.c
    framework/selftest/random_test.c)

# Builds an executable of engine test cases. Its entry point turns the engine's
# fatal conditions into test results and checks every case for leaks; see
# support/engine_test.h. The arguments go to tb_add_test_executable.
function(tb_add_tests target)
    tb_add_test_executable(${target}
        MAIN "${CMAKE_CURRENT_SOURCE_DIR}/support/engine_main.c"
        ${ARGN})
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/support/engine_memory.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/support/test_allocator.c")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/support"
        # The framework's own cases test its internal interfaces.
        "${CMAKE_CURRENT_SOURCE_DIR}/framework/src")
    tb_configure_target(${target})
endfunction()

tb_add_tests(tb_tests SOURCES ${TB_TEST_CASE_SOURCES} LIBRARIES TenaciousBadger::badger)

# Some behaviors cannot be cases of tb_tests because they end the process or
# must fail: the framework's own failure reports (framework/selftest/fixture),
# the engine's default handling of fatal conditions, and the test support's
# own checks. tb_probe holds those cases; a script per topic runs them in a
# child process and checks the outcome.
tb_add_tests(tb_probe NO_CTEST
    SOURCES core/assert_probe.c core/fmt_probe.c core/memory_probe.c support/support_probe.c
    LIBRARIES TenaciousBadger::badger)
function(tb_add_probe_test name script)
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
endfunction()
tb_add_probe_test(core_assert.default_handler_reports_and_aborts ExpectAssertAbort.cmake)
tb_add_probe_test(core_fmt.prints_to_the_standard_streams ExpectFormattedOutput.cmake)
tb_add_probe_test(core_memory.default_handler_reports_and_aborts ExpectOutOfMemoryAbort.cmake)
tb_add_probe_test(test_support.leaks_and_failure_injection ExpectTestSupport.cmake)

# Checks of the source text. They run with the tests so that a violation fails
# locally, before it fails in CI.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME source_checks.foundation
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_foundation.py")
add_test(NAME source_checks.foundation_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_foundation.py" --self-test)

if(TARGET burrow)
    add_test(NAME burrow.smoke COMMAND burrow)
    set_tests_properties(burrow.smoke PROPERTIES PASS_REGULAR_EXPRESSION "2 \\+ 3 = 5")
endif()

add_executable(tb_static_tests consumers/static/main.c)
target_link_libraries(tb_static_tests PRIVATE TenaciousBadger::badger_static)
tb_configure_target(tb_static_tests)
add_test(NAME consumers.static COMMAND tb_static_tests)

# Compiles engine sources into a test executable in place of linking the
# engine library, so that the tests reach what the library does not export.
function(tb_make_standalone target)
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
    target_compile_definitions(${target} PRIVATE TB_STATIC)
endfunction()

# The platform adapters are the layer beneath the foundation modules. Their
# tests are built from the selected adapter's sources and nothing else of the
# engine, which keeps the adapters free of the modules above them and lets a
# test call a function that is private to an adapter.
set(TB_PLATFORM_TEST_SOURCES
    platform/atomic_test.c platform/contract_test.c platform/thread_test.c
    platform/time_test.c)
# An adapter reports a broken contract through tb_assert_fail(), which the
# core defines. Executables built from an adapter alone get it from here, as
# a trap of the test framework.
set(TB_PLATFORM_TEST_SUPPORT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/support/platform_assert.c")
set(TB_PLATFORM_PRIVATE_DIRECTORY "")
if(WIN32)
    list(APPEND TB_PLATFORM_TEST_SOURCES
        platform/win32/counter_test.c platform/win32/thread_wait_test.c)
    set(TB_PLATFORM_PRIVATE_DIRECTORY "${PROJECT_SOURCE_DIR}/src/platform/win32")
endif()

# The arguments go to tb_add_test_executable.
function(tb_add_platform_tests target)
    tb_add_test_executable(${target} SOURCES ${TB_PLATFORM_TEST_SOURCES} ${ARGN})
    target_sources(${target} PRIVATE ${TB_PLATFORM_SOURCES} ${TB_PLATFORM_TEST_SUPPORT_SOURCES})
    if(TB_PLATFORM_PRIVATE_DIRECTORY)
        target_include_directories(${target} PRIVATE "${TB_PLATFORM_PRIVATE_DIRECTORY}")
    endif()
    tb_configure_target(${target})
    tb_make_standalone(${target})
endfunction()

tb_add_platform_tests(tb_platform_tests)

# A platform without an adapter of its own is built with the default one,
# whose every function reports "not implemented" and ends the process. It is
# standard C, so it is built and probed here on every platform: a function
# cannot join a platform interface without its default.
tb_add_test_executable(tb_platform_default_probe NO_CTEST SOURCES platform/default_probe.c)
target_sources(tb_platform_default_probe PRIVATE ${TB_PLATFORM_DEFAULT_SOURCES})
target_include_directories(tb_platform_default_probe PRIVATE
    "${TB_PLATFORM_DEFAULT_INCLUDE_DIRECTORY}")
tb_configure_target(tb_platform_default_probe)
tb_make_standalone(tb_platform_default_probe)
set(TB_PLATFORM_INTERFACE_HEADERS
    "${PROJECT_SOURCE_DIR}/include/badger/platform.h"
    "${PROJECT_SOURCE_DIR}/include/badger/thread.h")
list(JOIN TB_PLATFORM_INTERFACE_HEADERS "," tb_platform_interface_headers)
add_test(NAME platform_default.every_function_reports_not_implemented
    COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_platform_default_probe>"
        "-DTB_HEADERS=${tb_platform_interface_headers}"
        "-DTB_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformDefault.cmake")
add_test(NAME platform_selection.an_unsupported_platform_stops_the_build
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformSelection.cmake")

if(TB_SANITIZER)
    # Compile engine sources into standalone test executables. The shipping
    # shared library and burrow never acquire sanitizer dependencies. The
    # build has one kind of sanitizer, address or thread (cmake/Sanitizers.cmake),
    # and the same executables are built for either.
    function(tb_make_standalone_sanitized target)
        tb_make_standalone(${target})
        tb_enable_sanitizers(${target})
    endfunction()

    tb_add_tests(tb_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized
        SOURCES ${TB_TEST_CASE_SOURCES})
    target_sources(tb_tests_sanitized PRIVATE ${TB_CORE_SOURCES} ${TB_PLATFORM_SOURCES})
    tb_make_standalone_sanitized(tb_tests_sanitized)

    if(TB_SANITIZER STREQUAL "address")
        # Touching memory that an arena has released must end the process with a
        # sanitizer report, so it is probed from outside like the other fatal paths.
        tb_add_tests(tb_probe_sanitized NO_CTEST FRAMEWORK tb_test_sanitized
            SOURCES core/arena_probe.c)
        target_sources(tb_probe_sanitized PRIVATE ${TB_CORE_SOURCES} ${TB_PLATFORM_SOURCES})
        tb_make_standalone_sanitized(tb_probe_sanitized)
        add_test(NAME core_arena.released_memory_is_reported.sanitized
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_probe_sanitized>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectArenaPoison.cmake")
    endif()

    tb_add_platform_tests(tb_platform_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized)
    tb_enable_sanitizers(tb_platform_tests_sanitized)

    if(TB_SANITIZER STREQUAL "thread")
        # A run that reports no data race proves nothing unless the sanitizer
        # can see one. This probe races on purpose, and repeats the race behind
        # each thing the engine offers to prevent it; a script checks that the
        # first is reported and the others are not.
        tb_add_test_executable(tb_race_probe_sanitized NO_CTEST FRAMEWORK tb_test_sanitized
            SOURCES platform/race_probe.c)
        target_sources(tb_race_probe_sanitized PRIVATE
            ${TB_PLATFORM_SOURCES} ${TB_PLATFORM_TEST_SUPPORT_SOURCES})
        tb_configure_target(tb_race_probe_sanitized)
        tb_make_standalone(tb_race_probe_sanitized)
        tb_enable_sanitizers(tb_race_probe_sanitized)
        add_test(NAME platform_race.a_data_race_is_reported_and_synchronized_access_is_not.sanitized
            COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_race_probe_sanitized>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectDataRace.cmake")
    endif()
endif()
