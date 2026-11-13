# The in-house unit test framework. It is a development tool: never installed,
# never exported, and independent of the engine so it can test the engine's
# lowest layers.

# The framework's headers contain no compiler checks; the build selects
# compiler-specific support here, as it does for the engine's api.h.
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(TB_TEST_PRINTF_FORMAT_ATTRIBUTE
        "__attribute__((format(printf, format_index, arguments_index)))")
elseif(MSVC)
    # MSVC checks printf formats itself and has no attribute for user functions.
    set(TB_TEST_PRINTF_FORMAT_ATTRIBUTE "")
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_C_COMPILER_ID}")
endif()
configure_file(include/tb_test_config.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/tb_test_config.h" @ONLY)

if(WIN32)
    set(TB_TEST_PLATFORM_SOURCE src/platform/win32.c)
elseif(UNIX)
    set(TB_TEST_PLATFORM_SOURCE src/platform/posix.c)
else()
    message(FATAL_ERROR
        "The test framework has no platform adapter for ${CMAKE_SYSTEM_NAME}; "
        "add one under tests/framework/src/platform")
endif()

function(tb_test_add_framework_library target)
    add_library(${target} STATIC
        src/checks.c
        src/options.c
        src/pattern.c
        src/random.c
        src/report.c
        src/runner.c
        src/support.c
        "${TB_TEST_PLATFORM_SOURCE}")
    target_include_directories(${target} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    tb_configure_target(${target})
endfunction()

tb_test_add_framework_library(tb_test)
if(TB_ENABLE_SANITIZERS)
    # Standalone sanitizer executables link an instrumented copy, so the
    # framework's own code is checked along with the code under test.
    tb_test_add_framework_library(tb_test_sanitized)
    tb_enable_sanitizers(tb_test_sanitized)
endif()

include(cmake/TbTest.cmake)
add_subdirectory(selftest)
