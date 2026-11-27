# Media: bitmaps and sounds in memory, and the image and WAV files they are
# read from and written to.
set(TB_MEDIA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_bmp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_png.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_png_write.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sprite.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sprite_atlas.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sprite_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sprite_write.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sound.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sound_edit.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_codec.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_reader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/wav_writer.c"
    CACHE INTERNAL "Media sources")
target_sources(badger PRIVATE ${TB_MEDIA_SOURCES})
target_sources(badger_static PRIVATE ${TB_MEDIA_SOURCES})
