# The render hardware interface: one interface to every graphics API. The
# build selects the backends a platform has, and its list of them in its order
# of preference, as it selects the platform adapter.
set(TB_RHI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_recreate.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_stage.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_shader_library.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_commands.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/rhi_null.c")
tb_select_platform_adapter("${CMAKE_SYSTEM_NAME}" "${TB_USE_DEFAULT_PLATFORM_ADAPTER}"
    tb_rhi_platform)
list(APPEND TB_RHI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${tb_rhi_platform}/rhi_backends.c")
if(tb_rhi_platform STREQUAL "win32")
    list(APPEND TB_RHI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_commands.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_device.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_guids.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_resources.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d12/d3d12_swap_chain.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_commands.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_device.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_guids.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_resources.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/d3d11_swap_chain.c")
elseif(tb_rhi_platform STREQUAL "macos")
    list(APPEND TB_RHI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_pipeline.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_heaps.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_commands.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_device.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_resources.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/metal/metal_swap_chain.c")
endif()
set(TB_RHI_SOURCES ${TB_RHI_SOURCES} CACHE INTERNAL "Render hardware interface sources")
target_sources(badger PRIVATE ${TB_RHI_SOURCES})
target_sources(badger_static PRIVATE ${TB_RHI_SOURCES})
