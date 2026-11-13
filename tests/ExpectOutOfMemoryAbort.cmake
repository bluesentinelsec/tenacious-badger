# Runs the memory probe and checks the engine's default behavior when a
# fail-fast allocation cannot be satisfied: the request is reported on stderr
# and the process ends at once with a failure status.
execute_process(
    COMMAND "${TB_PROBE}" "memory_probe.exhausted_allocation_reports_and_aborts"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
    TIMEOUT 60)
set(report "status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")

if(NOT output MATCHES "before-fail-fast-allocation")
    message(FATAL_ERROR "The probe did not reach its fail-fast allocation: ${report}")
endif()
if(result STREQUAL "0" OR result MATCHES "timeout")
    message(FATAL_ERROR "An exhausted allocation must end the process promptly: ${report}")
endif()
if(output MATCHES "after-fail-fast-allocation")
    message(FATAL_ERROR "A fail-fast allocation must not return without memory: ${report}")
endif()
if(NOT diagnostic MATCHES "badger: out of memory allocating 3 x 64 bytes \\(alignment 0\\)")
    message(FATAL_ERROR "Unexpected out-of-memory diagnostic: ${report}")
endif()
