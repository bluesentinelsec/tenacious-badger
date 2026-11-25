# The interface: an immediate-mode GUI laid out in units and drawn through
# the renderer's canvas.
set(TB_UI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_editor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_paint.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_text_field.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_widgets.c"
    CACHE INTERNAL "Interface sources")
target_sources(badger PRIVATE ${TB_UI_SOURCES})
target_sources(badger_static PRIVATE ${TB_UI_SOURCES})
