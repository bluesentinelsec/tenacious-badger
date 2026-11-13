add_subdirectory(framework)

# Every test case of the project, in one executable: tb_tests. Add the source
# of a new test here. Suites are named <module>_<feature>, so one pattern
# selects a module (tb_tests "core_*") or a feature (tb_tests "core_sum.*").
set(TB_TEST_CASE_SOURCES
    core/assert_test.c
    core/memory_test.c
    core/sum_test.c
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
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support/engine_memory.c")
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
    SOURCES core/assert_probe.c core/memory_probe.c support/support_probe.c
    LIBRARIES TenaciousBadger::badger)
function(tb_add_probe_test name script)
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}" "-DTB_PROBE=$<TARGET_FILE:tb_probe>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
endfunction()
tb_add_probe_test(core_assert.default_handler_reports_and_aborts ExpectAssertAbort.cmake)
tb_add_probe_test(core_memory.default_handler_reports_and_aborts ExpectOutOfMemoryAbort.cmake)
tb_add_probe_test(test_support.leaks_and_failure_injection ExpectTestSupport.cmake)

# Checks of the source text. They run with the tests so that a violation fails
# locally, before it fails in CI.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME source_checks.allocations
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_allocations.py")
add_test(NAME source_checks.allocations_self_test
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_allocations.py" --self-test)

if(TARGET burrow)
    add_test(NAME burrow.smoke COMMAND burrow)
    set_tests_properties(burrow.smoke PROPERTIES PASS_REGULAR_EXPRESSION "2 \\+ 3 = 5")
endif()

add_executable(tb_static_tests consumers/static/main.c)
target_link_libraries(tb_static_tests PRIVATE TenaciousBadger::badger_static)
tb_configure_target(tb_static_tests)
add_test(NAME consumers.static COMMAND tb_static_tests)

function(tb_add_platform_test target suffix)
    if(WIN32)
        add_test(NAME platform.time${suffix} COMMAND ${target})
    else()
        add_test(NAME platform.fatal_stub${suffix}
            COMMAND "${CMAKE_COMMAND}"
                "-DTB_PROBE=$<TARGET_FILE:${target}>"
                "-DTB_PLATFORM=${TB_STUB_PLATFORM}"
                "-DTB_NATIVE_API=${TB_STUB_API}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectPlatformStub.cmake")
    endif()
endfunction()

add_executable(tb_platform_probe platform/time_probe.c)
target_link_libraries(tb_platform_probe PRIVATE TenaciousBadger::badger)
tb_configure_target(tb_platform_probe)
tb_add_platform_test(tb_platform_probe "")

if(TB_ENABLE_SANITIZERS)
    # Compile engine sources into standalone test executables. The shipping
    # shared library and burrow never acquire sanitizer dependencies.
    function(tb_make_standalone_sanitized target)
        target_include_directories(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/include" "${PROJECT_BINARY_DIR}/generated")
        target_compile_definitions(${target} PRIVATE TB_STATIC)
        tb_enable_sanitizers(${target})
    endfunction()

    tb_add_tests(tb_tests_sanitized
        SUFFIX ".sanitized" FRAMEWORK tb_test_sanitized
        SOURCES ${TB_TEST_CASE_SOURCES})
    target_sources(tb_tests_sanitized PRIVATE ${TB_CORE_SOURCES} ${TB_PLATFORM_SOURCES})
    tb_make_standalone_sanitized(tb_tests_sanitized)

    add_executable(tb_platform_sanitized platform/time_probe.c ${TB_PLATFORM_SOURCES})
    tb_configure_target(tb_platform_sanitized)
    tb_make_standalone_sanitized(tb_platform_sanitized)
    tb_add_platform_test(tb_platform_sanitized ".sanitized")
endif()
