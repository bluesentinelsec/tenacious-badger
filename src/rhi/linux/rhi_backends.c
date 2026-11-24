/* The backends of Linux and the Steam Deck, in the order `auto` tries them.
 * None is written yet; each takes its table in the change that writes it. */
#include "../rhi_backend.h"

static const tb_rhi_backend_entry tb_rhi_linux_backends[] = {
    {TB_RHI_BACKEND_VULKAN, NULL}, {TB_RHI_BACKEND_OPENGL, NULL}, {TB_RHI_BACKEND_SOFTWARE, NULL}};

static const tb_rhi_backend_list tb_rhi_linux = {"Linux", tb_rhi_linux_backends,
                                                 sizeof(tb_rhi_linux_backends) /
                                                     sizeof(tb_rhi_linux_backends[0])};

const tb_rhi_backend_list *tb_rhi_platform_backends(void)
{
    return &tb_rhi_linux;
}
