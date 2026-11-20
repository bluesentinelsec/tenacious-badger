# Runs the crash probe and checks the reports of tb_crash_catch(): a crash
# the system raises is reported on stderr as a line that says what happened,
# then the stack of the code that crashed, and the process ends at once with
# a failure status.
function(run_case case)
    execute_process(
        COMMAND "${TB_PROBE}" "crash_probe.${case}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${case}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
    if(NOT output MATCHES "before-crash")
        message(FATAL_ERROR "The probe did not reach its crash: ${report}")
    endif()
    if(result STREQUAL "0" OR result MATCHES "timeout")
        message(FATAL_ERROR "A crash must end the process promptly: ${report}")
    endif()
    if(output MATCHES "after-crash")
        message(FATAL_ERROR "A crash must not return to the code that crashed: ${report}")
    endif()
    set(diagnostic "${diagnostic}" PARENT_SCOPE)
    set(report "${report}" PARENT_SCOPE)
endfunction()

# What happened on a line, then at least two frames of the stack.
function(expect_crash case what)
    run_case(${case})
    if(NOT diagnostic MATCHES "(^|\n)crash: ${what}[^\n]*\r?\n    #0 [^\n]*\n    #1 ")
        message(FATAL_ERROR "Unexpected report of a crash: ${report}")
    endif()
    string(REGEX MATCHALL "crash: " reports "${diagnostic}")
    list(LENGTH reports count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "A crash must be reported once: ${report}")
    endif()
endfunction()

set(null_pointer "(access violation writing|segmentation fault|bus error)")
expect_crash(a_null_pointer_is_reported_with_its_stack "${null_pointer}")
expect_crash(abort_is_reported_with_its_stack "abort")
expect_crash(a_crash_on_another_thread_is_reported "${null_pointer}")
expect_crash(a_crash_holding_the_engines_lock_is_reported "${null_pointer}")

run_case(a_fatal_condition_is_reported_once)
if(NOT diagnostic MATCHES "(^|\n)probe: fatal after catching crashes\r?\n    #0 ")
    message(FATAL_ERROR "The fatal condition was not reported: ${report}")
endif()
if(diagnostic MATCHES "crash: ")
    message(FATAL_ERROR "A fatal condition must not be reported again as a crash: ${report}")
endif()
