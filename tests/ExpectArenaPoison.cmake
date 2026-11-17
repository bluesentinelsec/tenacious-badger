# Runs the arena probe of a sanitizer build and checks that AddressSanitizer
# knows which arena memory is in use: touching released or unused memory ends
# the process with a use-after-poison report, and valid use is left alone.

# Runs one probe case. Sets result, output, diagnostic, and report.
macro(run_probe_case name)
    execute_process(COMMAND "${TB_PROBE}" "arena_probe.${name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${name}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
endmacro()

foreach(name IN ITEMS
        read_after_reset
        write_after_rewind
        read_after_reset_of_a_fixed_arena
        read_after_reset_of_a_reserved_arena
        write_after_rewind_of_a_reserved_arena
        write_past_the_last_object)
    run_probe_case(${name})
    if(NOT output MATCHES "before-invalid-access")
        message(FATAL_ERROR "The probe did not reach its invalid access: ${report}")
    endif()
    if(output MATCHES "after-invalid-access")
        message(FATAL_ERROR "The invalid access went unnoticed: ${report}")
    endif()
    if(result STREQUAL "0" OR result MATCHES "timeout")
        message(FATAL_ERROR "The invalid access must end the process promptly: ${report}")
    endif()
    if(NOT diagnostic MATCHES "AddressSanitizer: use-after-poison")
        message(FATAL_ERROR "The invalid access was not reported as poisoned memory: ${report}")
    endif()
endforeach()

run_probe_case(valid_use_is_not_reported)
if(NOT result STREQUAL "0" OR NOT output MATCHES "valid-use-completed")
    message(FATAL_ERROR "Valid use of an arena must not be reported: ${report}")
endif()
