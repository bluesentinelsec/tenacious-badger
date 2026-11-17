cmake_minimum_required(VERSION 3.25)
project(BadgerFoundationConsumer LANGUAGES C)
find_package(TenaciousBadger CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Optional development policy: the CI driver supplies a copied signing module.
# Library consumption itself uses only the installed package and public API.
if(TB_CONSUMER_SIGNING_MODULE)
    include("${TB_CONSUMER_SIGNING_MODULE}")
endif()
enable_testing()
foreach(variant IN ITEMS shared static)
    add_executable(foundation_${variant} main.c)
    if(variant STREQUAL "shared")
        target_link_libraries(foundation_${variant} PRIVATE TenaciousBadger::badger)
        if(WIN32)
            add_custom_command(TARGET foundation_${variant} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:TenaciousBadger::badger>" "$<TARGET_FILE_DIR:foundation_${variant}>"
                VERBATIM)
        endif()
    else()
        target_link_libraries(foundation_${variant} PRIVATE TenaciousBadger::badger_static)
    endif()
    set_target_properties(foundation_${variant} PROPERTIES
        C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MSVC)
        target_compile_options(foundation_${variant} PRIVATE /W4 /WX /sdl /utf-8
            "$<$<CONFIG:Debug>:/Od>" "$<$<CONFIG:Debug>:/Zi>")
    else()
        target_compile_options(foundation_${variant} PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion
            -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
            -Wundef -Wcast-qual -Wwrite-strings -Wswitch-enum -Wdouble-promotion -Wvla
            "$<$<CONFIG:Debug>:-O0>" "$<$<CONFIG:Debug>:-g>" "$<$<CONFIG:Release>:-O3>")
    endif()
    if(COMMAND tb_sign_windows_target)
        tb_sign_windows_target(foundation_${variant})
    endif()
    add_test(NAME foundation.installed_${variant}
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
            "$<TARGET_FILE:foundation_${variant}>")
endforeach()
if(COMMAND tb_add_windows_signing_tests)
    tb_add_windows_signing_tests()
endif()
