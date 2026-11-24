/* The backends Windows builds: Direct3D 12, and the null backend. The others
 * join this list in the changes that write them. */
#include "../rhi_backend.h"

const tb_rhi_backend_table *tb_rhi_backend_table_of(tb_rhi_backend backend)
{
    switch (backend)
    {
    case TB_RHI_BACKEND_D3D12:
        return &tb_rhi_d3d12_backend;
    case TB_RHI_BACKEND_NULL:
        return &tb_rhi_null_backend;
    case TB_RHI_BACKEND_AUTOMATIC:
    case TB_RHI_BACKEND_D3D11:
    case TB_RHI_BACKEND_VULKAN:
    case TB_RHI_BACKEND_OPENGL:
    case TB_RHI_BACKEND_METAL:
    case TB_RHI_BACKEND_SOFTWARE:
    case TB_RHI_BACKEND_COUNT:
        break;
    }
    return NULL;
}
