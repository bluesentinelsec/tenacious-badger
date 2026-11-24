/* The backends of a platform without one of its own yet: the null backend.
 * Each graphics API joins this list in the change that writes it; until then,
 * choosing it is "not implemented". */
#include "../rhi_backend.h"

const tb_rhi_backend_table *tb_rhi_backend_table_of(tb_rhi_backend backend)
{
    switch (backend)
    {
    case TB_RHI_BACKEND_NULL:
        return &tb_rhi_null_backend;
    case TB_RHI_BACKEND_AUTOMATIC:
    case TB_RHI_BACKEND_D3D12:
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
