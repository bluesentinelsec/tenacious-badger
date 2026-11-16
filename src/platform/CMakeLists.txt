include("${PROJECT_SOURCE_DIR}/cmake/PlatformAdapter.cmake")
tb_select_platform_adapter("${CMAKE_SYSTEM_NAME}" "${TB_USE_DEFAULT_PLATFORM_ADAPTER}"
    tb_platform_adapter)

# The default adapter is described on every platform, because its tests are
# built on every platform. It becomes the engine's adapter only when chosen.
add_subdirectory(default)
if(NOT tb_platform_adapter STREQUAL "default")
    add_subdirectory(${tb_platform_adapter})
endif()

# Shared code consumes generated headers with no OS detection: how a public
# symbol is made visible, and how large the system's lock and condition
# variable are, which badger/thread.h needs to hold them in place.
if(tb_platform_adapter STREQUAL "linux" OR tb_platform_adapter STREQUAL "macos")
    configure_file(posix_api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
    configure_file(posix/thread_storage.h.in
        "${PROJECT_BINARY_DIR}/generated/badger/thread_storage.h" COPYONLY)
endif()
