# Runs a program with one argument and checks the status it exits with, and
# that its output, standard output and standard error together, matches.
#
#   TB_PROGRAM   The program.
#   TB_ARGUMENT  Its argument.
#   TB_STATUS    The status it must exit with.
#   TB_EXPECTED  A regular expression its output must match.
execute_process(COMMAND "${TB_PROGRAM}" "${TB_ARGUMENT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic TIMEOUT 60)
set(report "status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
if(NOT result STREQUAL "${TB_STATUS}")
    message(FATAL_ERROR "Expected status ${TB_STATUS}: ${report}")
endif()
if(NOT "${output}${diagnostic}" MATCHES "${TB_EXPECTED}")
    message(FATAL_ERROR "Expected output matching '${TB_EXPECTED}': ${report}")
endif()
