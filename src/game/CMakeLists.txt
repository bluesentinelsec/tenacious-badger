# The game loop: fixed ticks, frames between them, pausing, settings applied
# as they change, and quitting.
set(TB_GAME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/game_loop.c"
    CACHE INTERNAL "Game loop sources")
target_sources(badger PRIVATE ${TB_GAME_SOURCES})
target_sources(badger_static PRIVATE ${TB_GAME_SOURCES})
