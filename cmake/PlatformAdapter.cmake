# Chooses the platform adapter: the directory under src/platform whose sources
# implement the platform interfaces for the system being built for.
#
# Windows, Linux, and macOS each have an adapter that implements every
# interface. Any other system is unsupported and stops the build here, unless
# the default adapter was asked for: the one whose every function reports
# "not implemented" and ends the process, which a port starts from.
#
# tb_select_platform_adapter(<system-name> <use-default> <result-variable>)
#   <system-name>   CMAKE_SYSTEM_NAME of the build.
#   <use-default>   True to choose the default adapter whatever the system.
#   Sets <result-variable> to win32, linux, macos, or default.
function(tb_select_platform_adapter system_name use_default result_variable)
    if(use_default)
        set(adapter default)
    elseif(system_name STREQUAL "Windows")
        set(adapter win32)
    elseif(system_name STREQUAL "Linux")
        set(adapter linux)
    elseif(system_name STREQUAL "Darwin")
        set(adapter macos)
    else()
        message(FATAL_ERROR
            "Tenacious Badger does not support the platform '${system_name}'. "
            "It supports Windows, Linux, and macOS.\n"
            "To port it, add a platform adapter in src/platform/<name>/ that implements "
            "every platform interface, and choose it in cmake/PlatformAdapter.cmake; "
            "docs/platforms.md describes the interfaces.\n"
            "To build before the adapter is finished, configure with "
            "-DTB_USE_DEFAULT_PLATFORM_ADAPTER=ON. Every function of the default adapter "
            "reports \"not implemented\" and ends the process.")
    endif()
    set(${result_variable} "${adapter}" PARENT_SCOPE)
endfunction()
