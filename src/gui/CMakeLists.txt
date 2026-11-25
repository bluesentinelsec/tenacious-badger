# The retained GUI: widgets made once, laid out by a grid, and drawn through
# the renderer's canvas, on the layer it shares with the interface (src/ui).
set(TB_GUI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/gui.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_listbox.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_menu.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_popup.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_property.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_scroll.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_scrollbar.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_tooltip.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_widgets.c"
    CACHE INTERNAL "Retained GUI sources")
target_sources(badger PRIVATE ${TB_GUI_SOURCES})
target_sources(badger_static PRIVATE ${TB_GUI_SOURCES})
