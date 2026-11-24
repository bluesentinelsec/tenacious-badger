# The renderer: the render graph, and what draws through it.
set(TB_RENDER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/camera.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/canvas.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/canvas_batch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/canvas_text.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/image_pixels.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/light.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/material.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/mesh_shapes.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_graph.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skin_vertices.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/skinning.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sky.c"
    CACHE INTERNAL "Renderer sources")
target_sources(badger PRIVATE ${TB_RENDER_SOURCES})
target_sources(badger_static PRIVATE ${TB_RENDER_SOURCES})
