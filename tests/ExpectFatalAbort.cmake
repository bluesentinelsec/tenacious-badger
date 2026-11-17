# Runs the fatal probe and checks the engine's default handling of a fatal
# condition: the message and the call stack go to stderr, and the process ends
# at once with a failure status. A handler that returns ends it too.
function(expect_fatal case message)
    execute_process(
        COMMAND "${TB_PROBE}" "fatal_probe.${case}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${case}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
    if(NOT output MATCHES "before-fatal")
        message(FATAL_ERROR "The probe did not reach its fatal condition: ${report}")
    endif()
    if(result STREQUAL "0" OR result MATCHES "timeout")
        message(FATAL_ERROR "A fatal condition must end the process promptly: ${report}")
    endif()
    if(output MATCHES "after-fatal")
        message(FATAL_ERROR "A fatal condition must not return to its caller: ${report}")
    endif()
    # The message on a line of its own, without the newline it was given
    # twice, and then the stack, the innermost frame first.
    if(NOT diagnostic MATCHES "(^|\n)${message}\r?\n    #0 [^\n]*\n    #1 ")
        message(FATAL_ERROR "Unexpected report of a fatal condition: ${report}")
    endif()
    set(diagnostic "${diagnostic}" PARENT_SCOPE)
    set(report "${report}" PARENT_SCOPE)
endfunction()

expect_fatal(the_default_writes_the_report_and_ends_the_process "probe: the value was 42")
expect_fatal(a_handler_that_returns_does_not_keep_the_process_alive "probe: the value was 7")
if(NOT diagnostic MATCHES "handler-saw: probe: the value was 7")
    message(FATAL_ERROR "The handler was not given the report: ${report}")
endif()
