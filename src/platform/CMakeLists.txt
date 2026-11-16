if(WIN32)
    add_subdirectory(win32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_subdirectory(macos)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}; add a platform adapter")
endif()

# Shared code consumes generated headers with no OS detection: how a public
# symbol is made visible, and how large the system's lock and condition
# variable are, which badger/thread.h needs to hold them in place.
if(NOT WIN32)
    configure_file(posix_api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
    configure_file(posix/thread_storage.h.in
        "${PROJECT_BINARY_DIR}/generated/badger/thread_storage.h" COPYONLY)
endif()
