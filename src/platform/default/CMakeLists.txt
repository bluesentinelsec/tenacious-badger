# The default adapter: every platform interface for a system that has no
# adapter of its own. It is standard C, so it compiles anywhere, and each of
# its functions reports "not implemented" and ends the process.
set(TB_PLATFORM_DEFAULT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/crash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dxc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/error_stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/process.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/not_implemented.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/random.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stack.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thread.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/window.c"
    CACHE INTERNAL "Default platform adapter sources")

# The report names the system the build was for, which standard C cannot ask.
set(TB_PLATFORM_DEFAULT_INCLUDE_DIRECTORY "${PROJECT_BINARY_DIR}/generated/platform_default"
    CACHE INTERNAL "Directory of the default platform adapter's generated header")
configure_file(platform_name.h.in "${TB_PLATFORM_DEFAULT_INCLUDE_DIRECTORY}/platform_name.h"
    @ONLY)

if(tb_platform_adapter STREQUAL "default")
    set(TB_PLATFORM_SOURCES ${TB_PLATFORM_DEFAULT_SOURCES}
        CACHE INTERNAL "Selected platform adapter sources")
    foreach(target badger badger_static)
        target_sources(${target} PRIVATE ${TB_PLATFORM_SOURCES})
        target_include_directories(${target} PRIVATE "${TB_PLATFORM_DEFAULT_INCLUDE_DIRECTORY}")
    endforeach()
    configure_file(api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
    configure_file(thread_storage.h.in
        "${PROJECT_BINARY_DIR}/generated/badger/thread_storage.h" COPYONLY)
endif()
