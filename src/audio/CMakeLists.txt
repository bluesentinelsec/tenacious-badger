set(TB_AUDIO_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/mixer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mixer_render.c"
    CACHE INTERNAL "Audio mixer sources")
target_sources(badger PRIVATE ${TB_AUDIO_SOURCES})
target_sources(badger_static PRIVATE ${TB_AUDIO_SOURCES})
