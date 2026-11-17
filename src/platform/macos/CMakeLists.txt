set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/condition.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    # POSIX threads are the same on Linux and macOS, except where a condition
    # variable waits for a limited time: that part is condition.c.
    "${CMAKE_CURRENT_SOURCE_DIR}/../posix/thread.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})
