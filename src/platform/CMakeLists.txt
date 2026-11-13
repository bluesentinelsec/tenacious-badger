if(WIN32)
    add_subdirectory(win32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_subdirectory(macos)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}; add a platform adapter")
endif()

# Shared code consumes a generated visibility header with no OS detection.
if(NOT WIN32)
    configure_file(posix_api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
endif()
