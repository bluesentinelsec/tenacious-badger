# Media: sounds in memory, and the WAV files they are read from and written to.
set(TB_MEDIA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/sound.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sound_edit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_codec.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_reader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_writer.c"
    CACHE INTERNAL "Media sources")
target_sources(badger PRIVATE ${TB_MEDIA_SOURCES})
target_sources(badger_static PRIVATE ${TB_MEDIA_SOURCES})
