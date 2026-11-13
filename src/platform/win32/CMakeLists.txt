set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})
configure_file(api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
