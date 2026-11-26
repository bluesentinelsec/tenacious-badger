# Media: bitmaps and the image files they are read from, and later sounds,
# fonts and models.
set(TB_MEDIA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_bmp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bitmap_file.c"
    CACHE INTERNAL "Media sources")
target_sources(badger PRIVATE ${TB_MEDIA_SOURCES})
target_sources(badger_static PRIVATE ${TB_MEDIA_SOURCES})
