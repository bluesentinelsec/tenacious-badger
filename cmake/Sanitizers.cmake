if(TB_ENABLE_SANITIZERS AND MSVC)
    # MSVC's default runtime checks conflict with AddressSanitizer; ASan replaces
    # those checks in this development build. This does not suppress warnings.
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

function(tb_enable_sanitizers target)
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:/fsanitize=address>")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL:NO>")
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
            "$<$<CONFIG:Debug>:-fno-sanitize-recover=all>")
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
    else()
        message(FATAL_ERROR "Sanitizers are not configured for this compiler")
    endif()
endfunction()
