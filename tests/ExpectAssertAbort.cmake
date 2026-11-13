# Runs the assertion probe and checks the engine's default failure behavior:
# with assertions enabled the process reports the failure and aborts; with
# assertions compiled out the same code runs to completion.
execute_process(COMMAND "${TB_PROBE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
    TIMEOUT 60)
set(report "status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")

if(output MATCHES "assertions=disabled")
    if(NOT result STREQUAL "0" OR NOT output MATCHES "after-assert")
        message(FATAL_ERROR "A disabled assertion must not stop the program: ${report}")
    endif()
    return()
endif()

if(NOT output MATCHES "assertions=enabled")
    message(FATAL_ERROR "The probe did not report its assertion setting: ${report}")
endif()
if(result STREQUAL "0" OR result MATCHES "timeout")
    message(FATAL_ERROR "A failed assertion must abort the process promptly: ${report}")
endif()
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
