#include "badger/dxc.h"

const char *tb_dxc_result_name(tb_dxc_result result)
{
    switch (result)
    {
    case TB_DXC_OK:
        return "ok";
    case TB_DXC_INVALID:
        return "invalid";
    case TB_DXC_UNAVAILABLE:
        return "unavailable";
    case TB_DXC_FAILED:
        return "failed";
    case TB_DXC_NO_MEMORY:
        return "no memory";
    }
    return "unknown result";
}
