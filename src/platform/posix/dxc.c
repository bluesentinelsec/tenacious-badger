/* DXC on Linux and macOS: unavailable. DXIL runs only on D3D12, which only
 * Windows has, so these systems lack the capability; no work is missing. */
#include "badger/dxc.h"

#include "badger/fmt.h"

#include <stddef.h>

static tb_dxc_result unavailable(tb_dxc_error *error)
{
    if (error != NULL)
    {
        error->result = TB_DXC_UNAVAILABLE;
        (void)tb_fmt_buffer(error->message, sizeof(error->message),
                            "DXC compiles DXIL for D3D12, which runs only on Windows; this "
                            "system's backends need no DXIL");
    }
    return TB_DXC_UNAVAILABLE;
}

tb_dxc_result tb_dxc_open(tb_dxc **dxc, tb_dxc_error *error)
{
    *dxc = NULL;
    return unavailable(error);
}

void tb_dxc_close(tb_dxc *dxc)
{
    (void)dxc;
}

tb_dxc_result tb_dxc_compile(tb_dxc *dxc, const tb_dxc_request *request, tb_dxc_output *output,
                             tb_dxc_error *error)
{
    /* coverage-off: tb_dxc_open() opens no DXC here, so nothing can call this */
    const tb_dxc_output empty = {0};
    (void)dxc;
    (void)request;
    *output = empty;
    return unavailable(error);
    /* coverage-on */
}

void tb_dxc_release(tb_dxc_output *output)
{
    const tb_dxc_output empty = {0};
    *output = empty;
}
