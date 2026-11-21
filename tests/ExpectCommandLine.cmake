# Runs the command-line probe and checks that tb_cli_finish() writes help and
# the version to standard output, and an error with its usage to standard
# error, each to its stream alone.
function(run_case name)
    execute_process(
        COMMAND "${TB_PROBE}" "cli_probe.${name}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
        TIMEOUT 60)
    set(report "${name}: status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "The probe case failed: ${report}")
    endif()
    set(output "${output}" PARENT_SCOPE)
    set(diagnostic "${diagnostic}" PARENT_SCOPE)
    set(report "${report}" PARENT_SCOPE)
endfunction()

run_case(help_goes_to_standard_output)
if(NOT output MATCHES "^usage: probe \\[options\\] <command> \\.\\.\\.\r?\n")
    message(FATAL_ERROR "The help does not begin with its usage: ${report}")
endif()
if(NOT output MATCHES "  -v, --verbose +Say more\\.\r?\n" OR
   NOT output MATCHES "  deploy +Deploy it\\.\r?\n")
    message(FATAL_ERROR "The help lacks its options or commands: ${report}")
endif()
if(diagnostic MATCHES "usage:")
    message(FATAL_ERROR "The help reached standard error: ${report}")
endif()

run_case(version_goes_to_standard_output)
if(NOT output MATCHES "^probe 9\\.8\\.7\r?\n")
    message(FATAL_ERROR "The version was not written: ${report}")
endif()

run_case(an_error_goes_to_standard_error_with_the_usage)
if(NOT diagnostic MATCHES "usage: probe deploy \\[options\\]\r?\nprobe deploy: error: unknown option '--dry-rn'; did you mean '--dry-run'\\?\r?\n")
    message(FATAL_ERROR "The error was not written with its usage: ${report}")
endif()
if(output MATCHES "usage:|error:")
    message(FATAL_ERROR "The error reached standard output: ${report}")
endif()
