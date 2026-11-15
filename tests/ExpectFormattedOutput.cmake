# Runs the formatting probe and checks that tb_fmt_print writes to standard
# output and tb_fmt_print_error to standard error, each exactly what was
# formatted and nothing else.
execute_process(
    COMMAND "${TB_PROBE}" "fmt_probe.prints_to_standard_output_and_standard_error"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
    TIMEOUT 60)
set(report "status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")

if(NOT result STREQUAL "0")
    message(FATAL_ERROR "The formatting probe failed: ${report}")
endif()
if(NOT output MATCHES "out: badger #007 \\[0xff\\]\r?\nboth: 100%\r?\n\\[ +end\\]\r?\n")
    message(FATAL_ERROR "Standard output did not receive the formatted text: ${report}")
endif()
if(NOT diagnostic MATCHES "err: badger #008 \\[0x1000\\]\r?\nboth: 100%\r?\n")
    message(FATAL_ERROR "Standard error did not receive the formatted text: ${report}")
endif()
if(output MATCHES "err:" OR diagnostic MATCHES "out:")
    message(FATAL_ERROR "Text was written to the wrong stream: ${report}")
endif()
string(REGEX MATCH "\\[ +end\\]" padded "${output}")
string(LENGTH "${padded}" padded_length)
if(NOT padded_length EQUAL 302)
    message(FATAL_ERROR "Long output did not arrive whole (${padded_length} bytes): ${report}")
endif()
