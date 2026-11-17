set(TB_PLATFORM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/abort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/atomic.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/thread.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/time.c"
    # Shared by every adapter that names threads.
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/name.c"
    CACHE INTERNAL "Selected platform adapter sources")
target_sources(badger PRIVATE ${TB_PLATFORM_SOURCES})
target_sources(badger_static PRIVATE ${TB_PLATFORM_SOURCES})
configure_file(api.h.in "${PROJECT_BINARY_DIR}/generated/badger/api.h" @ONLY)
configure_file(thread_storage.h.in
    "${PROJECT_BINARY_DIR}/generated/badger/thread_storage.h" COPYONLY)
