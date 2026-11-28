/* macOS tries the native Metal device first. */
#include "../metal/metal_internal.h"

static const tb_rhi_backend_entry tb_rhi_macos_backends[] = {
    {TB_RHI_BACKEND_METAL, &tb_rhi_metal_backend},
    {TB_RHI_BACKEND_OPENGL, NULL},
    {TB_RHI_BACKEND_SOFTWARE, NULL}};

static const tb_rhi_backend_list tb_rhi_macos = {"macOS", tb_rhi_macos_backends,
                                                 sizeof(tb_rhi_macos_backends) /
                                                     sizeof(tb_rhi_macos_backends[0])};

const tb_rhi_backend_list *tb_rhi_platform_backends(void)
{
    return &tb_rhi_macos;
}
