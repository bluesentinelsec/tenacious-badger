# Runs probe cases that misbehave on purpose and checks that the engine test
# support (tests/support) catches them: the leak check after each case and the
# scope of allocation failure injection.

# Runs probe cases. Sets result, output, and report.
macro(run_probe_cases)
    execute_process(COMMAND "${TB_PROBE}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${ARGN}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
endmacro()

run_probe_cases("support_probe.leaked_block_fails_the_case")
if(NOT result STREQUAL "1")
    message(FATAL_ERROR "A case that leaks must fail: ${report}")
endif()
if(NOT output MATCHES
        "support_probe\\.c[(:][0-9]+[):]+ error: support_probe\\.leaked_block_fails_the_case: TB_TEST_FAIL: memory leak: 2 blocks allocated by this case and never freed")
    message(FATAL_ERROR "The leak was not reported at the case with its size: ${report}")
endif()

run_probe_cases("support_probe.failed_case_is_not_checked_for_leaks")
if(NOT result STREQUAL "1" OR NOT output MATCHES "1 of 1 checks failed")
    message(FATAL_ERROR "The case must fail on its own check only: ${report}")
endif()
if(output MATCHES "memory leak")
    message(FATAL_ERROR "A failed case must not also be reported as leaking: ${report}")
endif()

run_probe_cases("support_probe.failure_injection_is_left_on"
    "support_probe.next_case_allocates_normally")
if(NOT result STREQUAL "0" OR NOT output MATCHES "2 cases: 2 passed")
    message(FATAL_ERROR "Failure injection must end with the case that started it: ${report}")
endif()
