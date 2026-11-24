/* The backends of Windows, in the order `auto` tries them: Direct3D 12 first,
 * then those still to be written. Each takes its table in the change that
 * writes it. */
#include "../rhi_backend.h"

static const tb_rhi_backend_entry tb_rhi_windows_backends[] = {
    {TB_RHI_BACKEND_D3D12, &tb_rhi_d3d12_backend},
    {TB_RHI_BACKEND_D3D11, NULL},
    {TB_RHI_BACKEND_VULKAN, NULL},
    {TB_RHI_BACKEND_OPENGL, NULL},
    {TB_RHI_BACKEND_SOFTWARE, NULL}};

static const tb_rhi_backend_list tb_rhi_windows = {"Windows", tb_rhi_windows_backends,
                                                   sizeof(tb_rhi_windows_backends) /
                                                       sizeof(tb_rhi_windows_backends[0])};

const tb_rhi_backend_list *tb_rhi_platform_backends(void)
{
    return &tb_rhi_windows;
}
