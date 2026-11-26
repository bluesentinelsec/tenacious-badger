#include "badger/d3dcompiler.h"

const char *tb_d3dcompiler_result_name(tb_d3dcompiler_result result)
{
    switch (result)
    {
    case TB_D3DCOMPILER_OK:
        return "ok";
    case TB_D3DCOMPILER_INVALID:
        return "invalid";
    case TB_D3DCOMPILER_UNAVAILABLE:
        return "unavailable";
    case TB_D3DCOMPILER_FAILED:
        return "failed";
    case TB_D3DCOMPILER_NO_MEMORY:
        return "no memory";
    }
    return "unknown result";
}
