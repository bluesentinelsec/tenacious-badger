add_custom_target(fmt
    COMMAND "${CMAKE_COMMAND}" -DTB_FORMAT_MODE=write
        -P "${PROJECT_SOURCE_DIR}/cmake/Format.cmake"
    VERBATIM)
add_custom_target(check-format
    COMMAND "${CMAKE_COMMAND}" -DTB_FORMAT_MODE=check
        -P "${PROJECT_SOURCE_DIR}/cmake/Format.cmake"
    VERBATIM)
