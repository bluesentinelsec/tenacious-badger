#include "badger/dxc.h"

#include "not_implemented.h"

/* Standard C cannot load DXC: what a library is, and whether a system runs
 * DXIL at all, is each system's own. */

tb_dxc_result tb_dxc_open(tb_dxc **dxc, tb_dxc_error *error)
{
    (void)dxc;
    (void)error;
    tb_default_not_implemented("platform.dxc", "tb_dxc_open");
}

void tb_dxc_close(tb_dxc *dxc)
{
    (void)dxc;
    tb_default_not_implemented("platform.dxc", "tb_dxc_close");
}

tb_dxc_result tb_dxc_compile(tb_dxc *dxc, const tb_dxc_request *request, tb_dxc_output *output,
                             tb_dxc_error *error)
{
    (void)dxc;
    (void)request;
    (void)output;
    (void)error;
    tb_default_not_implemented("platform.dxc", "tb_dxc_compile");
}

void tb_dxc_release(tb_dxc_output *output)
{
    (void)output;
    tb_default_not_implemented("platform.dxc", "tb_dxc_release");
}
