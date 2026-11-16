# Checks how the build chooses a platform adapter: Windows, Linux, and macOS
# each get their own, and any other platform stops the build with a message
# that names it and says what to do, unless the default adapter is asked for.
#
# The script runs itself as a child to see the build stop: TB_UNSUPPORTED_SYSTEM
# is set in the child, which then makes the one selection that must fail.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PlatformAdapter.cmake")

if(DEFINED TB_UNSUPPORTED_SYSTEM)
    tb_select_platform_adapter("${TB_UNSUPPORTED_SYSTEM}" OFF adapter)
    message(STATUS "selected ${adapter}")
    return()
endif()

function(expect_adapter system_name use_default expected)
    tb_select_platform_adapter("${system_name}" "${use_default}" adapter)
    if(NOT adapter STREQUAL expected)
        message(FATAL_ERROR
            "${system_name} with the default adapter ${use_default}: expected the adapter "
            "'${expected}', got '${adapter}'")
    endif()
endfunction()

expect_adapter(Windows OFF win32)
expect_adapter(Linux OFF linux)
expect_adapter(Darwin OFF macos)

# The default adapter is chosen only when asked for, and then on any system.
expect_adapter(Haiku ON default)
expect_adapter(Windows ON default)
expect_adapter(Linux ON default)
expect_adapter(Darwin ON default)

# Systems close to a supported one are not it.
foreach(system_name Haiku FreeBSD Android iOS Emscripten WindowsStore windows linux "")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DTB_UNSUPPORTED_SYSTEM=${system_name}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostic)
    set(report "'${system_name}': status '${result}'\n--- stdout:\n${output}\n--- stderr:\n${diagnostic}")
    if(result STREQUAL "0")
        message(FATAL_ERROR "An unsupported platform must stop the build: ${report}")
    endif()
    # CMake breaks the lines of a message to fit the terminal.
    string(REGEX REPLACE "[ \r\n]+" " " diagnostic "${diagnostic}")
    string(FIND "${diagnostic}" "does not support the platform '${system_name}'" names_platform)
    string(FIND "${diagnostic}" "src/platform/<name>/" names_directory)
    string(FIND "${diagnostic}" "-DTB_USE_DEFAULT_PLATFORM_ADAPTER=ON" names_option)
    if(names_platform EQUAL -1 OR names_directory EQUAL -1 OR names_option EQUAL -1)
        message(FATAL_ERROR
            "The message must name the platform, the adapter directory, and the option that "
            "builds with the default adapter: ${report}")
    endif()
endforeach()
