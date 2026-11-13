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

tb_add_test_executable(tb_test_fixture NO_CTEST SOURCES fixture/fixture_tests.c)
tb_configure_target(tb_test_fixture)
tb_test_add_scenarios(tb_test_fixture "")

if(TB_ENABLE_SANITIZERS)
    # The failure paths leave cases with non-local jumps; run them instrumented.
    tb_add_test_executable(tb_test_fixture_sanitized NO_CTEST
        FRAMEWORK tb_test_sanitized
        SOURCES fixture/fixture_tests.c)
    tb_configure_target(tb_test_fixture_sanitized)
    tb_enable_sanitizers(tb_test_fixture_sanitized)
    tb_test_add_scenarios(tb_test_fixture_sanitized ".sanitized")
endif()
