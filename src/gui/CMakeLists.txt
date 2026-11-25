# The retained GUI: widgets made once, laid out by a grid, and drawn through
# the renderer's canvas, on the layer it shares with the interface (src/ui).
set(TB_GUI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/gui.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_choice.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_combobox.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_entry.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_field.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_file.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_input.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_names.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_path.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_property.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_range.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_spinbox.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_values.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_var.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_watch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gui_widgets.c"
    CACHE INTERNAL "Retained GUI sources")
target_sources(badger PRIVATE ${TB_GUI_SOURCES})
target_sources(badger_static PRIVATE ${TB_GUI_SOURCES})
