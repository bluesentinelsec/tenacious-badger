set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})
set(TB_STUB_PLATFORM "linux" CACHE INTERNAL "Fatal stub platform")
set(TB_STUB_API "clock_gettime" CACHE INTERNAL "Planned native time API")
