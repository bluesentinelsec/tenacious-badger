# Runs the assertion probe and checks the engine's default failure behavior:
# the failure is reported on stderr and the process ends at once with a failure
# status. A run that waits on an error dialog shows up here as a timeout.

# Runs one probe case. Sets result, output, diagnostic, and report.
macro(run_probe_case case_name)
    execute_process(COMMAND "${TB_PROBE}" "${case_name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report
        "${case_name}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
endmacro()

function(expect_prompt_abort)
    if(result STREQUAL "0" OR result MATCHES "timeout")
        message(FATAL_ERROR "A failed assertion must end the process promptly: ${report}")
    endif()
endfunction()

# The function behind the macros aborts in every build.
run_probe_case("assert_probe.failure_function_aborts_in_every_build")
expect_prompt_abort()
if(NOT diagnostic MATCHES "probe_file\\.c:77: assertion failed in probe_function: probe == call")
    message(FATAL_ERROR "Unexpected assertion diagnostic: ${report}")
endif()

# The macros abort when assertions are enabled and do nothing otherwise.
run_probe_case("assert_probe.failed_macro_reports_and_aborts")
if(output MATCHES "assertions=disabled")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "after-assert")
        message(FATAL_ERROR "A disabled assertion must not stop the program: ${report}")
    endif()
    return()
endif()
if(NOT output MATCHES "assertions=enabled")
    message(FATAL_ERROR "The probe did not report its assertion setting: ${report}")
endif()
expect_prompt_abort()
if(output MATCHES "after-assert")
    message(FATAL_ERROR "Code after a failed assertion must not run: ${report}")
endif()
if(NOT output MATCHES "assert-line=([0-9]+)")
    message(FATAL_ERROR "The probe did not report the line of its assertion: ${report}")
endif()
set(expected
    "assert_probe\\.c:${CMAKE_MATCH_1}: assertion failed in [A-Za-z0-9_]+: two \\+ two == 5 \\(arithmetic is intact\\)")
if(NOT diagnostic MATCHES "${expected}")
    message(FATAL_ERROR "Unexpected assertion diagnostic, wanted ${expected}: ${report}")
endif()
