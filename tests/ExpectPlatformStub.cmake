execute_process(COMMAND "${TB_PROBE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic
    TIMEOUT 10)
if(NOT "${result}" STREQUAL "1")
    message(FATAL_ERROR "Expected fatal stub exit 1, got '${result}': ${output}${diagnostic}")
endif()
set(expected "platform.time: tb_platform_ticks on ${TB_PLATFORM} is not implemented; implement ${TB_NATIVE_API} in src/platform/${TB_PLATFORM}/time.c or run the Windows build")
string(STRIP "${diagnostic}" diagnostic)
if(NOT diagnostic STREQUAL expected)
    message(FATAL_ERROR "Unexpected fatal stub diagnostic: ${diagnostic}")
endif()
