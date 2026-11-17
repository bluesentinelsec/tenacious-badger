foreach(fault overrun underrun)
    execute_process(COMMAND "${TB_PROBE}" "memory_debug_probe.${fault}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic TIMEOUT 30)
    if(result STREQUAL "0" OR result MATCHES "timeout")
        message(FATAL_ERROR "${fault} must terminate: ${result}\n${output}\n${diagnostic}")
    endif()
    if(TB_RAW)
        if(NOT diagnostic MATCHES "AddressSanitizer.*heap-buffer-overflow")
            message(FATAL_ERROR "Sanitizer missed ${fault}: ${diagnostic}")
        endif()
    else()
        if(fault STREQUAL "overrun")
            set(direction after)
        else()
            set(direction before)
        endif()
        if(NOT diagnostic MATCHES "byte 1 ${direction} block 1" OR
           NOT diagnostic MATCHES "bytes 8" OR
           NOT diagnostic MATCHES "label probe" OR
           NOT diagnostic MATCHES "allocated at allocation.c:42")
            message(FATAL_ERROR "Incorrect ${fault} diagnostic: ${diagnostic}")
        endif()
    endif()
endforeach()
if(TB_RAW)
    return()
endif()
execute_process(COMMAND "${TB_PROBE}" "memory_debug_probe.returning_handler"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostic TIMEOUT 30)
if(result STREQUAL "0" OR result MATCHES "timeout" OR
   NOT diagnostic MATCHES "returning-handler-called")
    message(FATAL_ERROR "Returning handler did not abort: ${result}\n${diagnostic}")
endif()
execute_process(COMMAND "${TB_PROBE}" "memory_debug_probe.report"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostic TIMEOUT 30)
if(NOT result STREQUAL "0" OR
   NOT diagnostic MATCHES "blocks 40, bytes 820" OR
   NOT diagnostic MATCHES "label module-0: blocks 1, bytes 1" OR
   NOT diagnostic MATCHES "other labels: blocks 8, bytes 292" OR
   NOT diagnostic MATCHES "and more blocks: 8" OR
   NOT diagnostic MATCHES "allocated at allocation.c:42" OR
   NOT diagnostic MATCHES "blocks 0, bytes 0; at the most: blocks 0, bytes 0")
    message(FATAL_ERROR "Incorrect allocation report: ${result}\n${diagnostic}")
endif()
