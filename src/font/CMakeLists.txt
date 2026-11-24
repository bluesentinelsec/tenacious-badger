# Fonts: a reader of TrueType files, glyphs rasterized from them into an
# atlas, and the fonts the engine carries.
set(TB_FONT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/font.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_atlas.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_catalog.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_cmap.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_embedded.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_glyf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_kern.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_name.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/font_raster.c"
    CACHE INTERNAL "Font sources")
target_sources(badger PRIVATE ${TB_FONT_SOURCES})
target_sources(badger_static PRIVATE ${TB_FONT_SOURCES})

# The fonts' compressed data (cmake/EmbedFonts.cmake), compiled once for both.
tb_embed_fonts()
target_sources(badger PRIVATE ${TB_FONT_DATA_OBJECTS})
target_sources(badger_static PRIVATE ${TB_FONT_DATA_OBJECTS})
