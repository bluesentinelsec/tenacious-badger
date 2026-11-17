# The job system: worker threads and the tasks they run.
set(TB_JOBS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/deque.c"
    CACHE INTERNAL "Job system sources")
target_sources(badger PRIVATE ${TB_JOBS_SOURCES})
target_sources(badger_static PRIVATE ${TB_JOBS_SOURCES})
