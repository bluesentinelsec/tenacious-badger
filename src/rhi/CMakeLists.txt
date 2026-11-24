# The render hardware interface: one interface to every graphics API. The
# build selects the backends a platform has.
set(TB_RHI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_commands.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_null.c")
if(WIN32)
    list(APPEND TB_RHI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/win32/rhi_backends.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_commands.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_device.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_guids.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_resources.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_swap_chain.c")
else()
    list(APPEND TB_RHI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/default/rhi_backends.c")
endif()
set(TB_RHI_SOURCES ${TB_RHI_SOURCES} CACHE INTERNAL "Render hardware interface sources")
target_sources(badger PRIVATE ${TB_RHI_SOURCES})
target_sources(badger_static PRIVATE ${TB_RHI_SOURCES})
