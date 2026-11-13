function(tb_configure_target target)
    tb_sign_windows_target(${target})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C>:/W4;/WX;/sdl;/utf-8>"
            "$<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Debug>>:/Od;/Zi>")
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
            -Wundef -Wcast-qual -Wwrite-strings -Wswitch-enum
            -Wdouble-promotion -Wvla
            "$<$<CONFIG:Debug>:-O0>" "$<$<CONFIG:Debug>:-g>"
            "$<$<CONFIG:Release>:-O3>")
    else()
        message(FATAL_ERROR "Unsupported compiler: ${CMAKE_C_COMPILER_ID}")
    endif()
endfunction()

function(tb_strip_release target)
    if(NOT MSVC)
        if(NOT CMAKE_STRIP)
            message(FATAL_ERROR "The native strip tool is required for Release builds")
        endif()
        if(APPLE)
            set(strip_flags -S -x)
        else()
            set(strip_flags --strip-unneeded)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                "-DTB_CONFIG=$<CONFIG>"
                "-DTB_STRIP=${CMAKE_STRIP}"
                "-DTB_STRIP_FLAGS=${strip_flags}"
                "-DTB_BINARY=$<TARGET_FILE:${target}>"
                -P "${PROJECT_SOURCE_DIR}/cmake/StripRelease.cmake"
            VERBATIM)
    endif()
endfunction()
