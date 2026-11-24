# The render hardware interface: one interface to every graphics API.
set(TB_RHI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_backends.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_commands.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_null.c"
    CACHE INTERNAL "Render hardware interface sources")
target_sources(badger PRIVATE ${TB_RHI_SOURCES})
target_sources(badger_static PRIVATE ${TB_RHI_SOURCES})
