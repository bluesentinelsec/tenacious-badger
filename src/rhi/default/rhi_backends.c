/* The backends of a platform the engine does not support: none but the null
 * backend, which every platform has. A port lists its own, in its order of
 * preference, in a source of its own beside win32/, linux/ and macos/; until
 * then, choosing any other is "not implemented". */
#include "../rhi_backend.h"

static const tb_rhi_backend_list tb_rhi_unsupported = {"this platform", NULL, 0};

const tb_rhi_backend_list *tb_rhi_platform_backends(void)
{
    return &tb_rhi_unsupported;
}
