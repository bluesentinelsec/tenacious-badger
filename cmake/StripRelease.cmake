if(TB_CONFIG STREQUAL "Release")
    execute_process(COMMAND "${TB_STRIP}" ${TB_STRIP_FLAGS} "${TB_BINARY}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
