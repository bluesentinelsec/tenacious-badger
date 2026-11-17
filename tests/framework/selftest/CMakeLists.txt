# The framework tests itself in two ways. Cases that must pass (*_test.c) are
# part of the project's test executable, tb_tests, like any other test. Behavior
# that only shows when a case fails (reports, exit statuses, fail-fast) is
# checked from the outside, by running a fixture executable whose cases fail on
# purpose. That fixture is necessarily a separate program.

set(scenario_script "${CMAKE_CURRENT_SOURCE_DIR}/RunScenario.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scenario_script}")

# Registers one CTest test per `function(scenario_<name>)` in RunScenario.cmake,
# run against the given fixture executable, so adding a function is enough.
function(tb_test_add_scenarios fixture suffix)
    file(STRINGS "${scenario_script}" definitions REGEX "^function\\(scenario_")
    foreach(definition IN LISTS definitions)
        string(REGEX REPLACE "^function\\(scenario_([A-Za-z0-9_]+)\\).*$" "\\1" scenario
            "${definition}")
        add_test(NAME framework_scenario.${scenario}${suffix}
            COMMAND "${CMAKE_COMMAND}"
                "-DTB_SCENARIO=${scenario}"
                "-DTB_FIXTURE=$<TARGET_FILE:${fixture}>"
                "-DTB_FIXTURE_CASES=${CMAKE_CURRENT_BINARY_DIR}/${fixture}_cases.cmake"
                "-DTB_SCAN_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TbTestScan.cmake"
                "-DTB_WORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/scenarios${suffix}"
                -P "${scenario_script}")
    endforeach()
endfunction()

tb_add_test_executable(tb_test_fixture NO_CTEST
    MAIN fixture/fixture_main.c
    SOURCES fixture/fixture_tests.c)
tb_configure_target(tb_test_fixture)
tb_test_add_scenarios(tb_test_fixture "")

if(TB_SANITIZER STREQUAL "address")
    # The failure paths leave cases with non-local jumps; run them instrumented.
    # They run on one thread, so ThreadSanitizer has nothing to find in them.
    tb_add_test_executable(tb_test_fixture_sanitized NO_CTEST
        FRAMEWORK tb_test_sanitized
        MAIN fixture/fixture_main.c
        SOURCES fixture/fixture_tests.c)
    tb_configure_target(tb_test_fixture_sanitized)
    tb_enable_sanitizers(tb_test_fixture_sanitized)
    tb_test_add_scenarios(tb_test_fixture_sanitized ".sanitized")
endif()

if(WIN32)
    set(crash_native fixture/crash_win32.c)
else()
    set(crash_native fixture/crash_posix.c)
endif()
tb_add_test_executable(tb_test_crash_fixture NO_CTEST
    MAIN fixture/crash_main.c SOURCES fixture/crash_tests.c)
target_sources(tb_test_crash_fixture PRIVATE ${crash_native})
tb_configure_target(tb_test_crash_fixture)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME framework_crash.native_status_and_identity
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/crash_test.py"
        "$<TARGET_FILE:tb_test_crash_fixture>")
if(TB_SANITIZER STREQUAL "address")
    tb_add_test_executable(tb_test_crash_fixture_sanitized NO_CTEST
        FRAMEWORK tb_test_sanitized MAIN fixture/crash_main.c SOURCES fixture/crash_tests.c)
    target_sources(tb_test_crash_fixture_sanitized PRIVATE ${crash_native})
    tb_configure_target(tb_test_crash_fixture_sanitized)
    tb_enable_sanitizers(tb_test_crash_fixture_sanitized)
    add_test(NAME framework_crash.sanitizer_report_is_preserved.sanitized
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/crash_test.py"
            "$<TARGET_FILE:tb_test_crash_fixture_sanitized>" sanitized)
endif()
