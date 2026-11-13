target_sources(badger PRIVATE time.c)
target_sources(badger_static PRIVATE time.c)
configure_file(api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
set(TB_PLATFORM_TIME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/time.c" CACHE INTERNAL "Selected time adapter")
