#include "badger/d3dcompiler.h"

#include "not_implemented.h"

/* Standard C cannot load D3DCompiler: what a library is, and whether a
 * system runs DXBC at all, is each system's own. */

tb_d3dcompiler_result tb_d3dcompiler_open(tb_d3dcompiler **compiler, tb_d3dcompiler_error *error)
{
    (void)compiler;
    (void)error;
    tb_default_not_implemented("platform.d3dcompiler", "tb_d3dcompiler_open");
}

void tb_d3dcompiler_close(tb_d3dcompiler *compiler)
{
    (void)compiler;
    tb_default_not_implemented("platform.d3dcompiler", "tb_d3dcompiler_close");
}

tb_d3dcompiler_result tb_d3dcompiler_compile(tb_d3dcompiler *compiler,
                                             const tb_d3dcompiler_request *request,
                                             tb_d3dcompiler_output *output,
                                             tb_d3dcompiler_error *error)
{
    (void)compiler;
    (void)request;
    (void)output;
    (void)error;
    tb_default_not_implemented("platform.d3dcompiler", "tb_d3dcompiler_compile");
}

void tb_d3dcompiler_release(tb_d3dcompiler_output *output)
{
    (void)output;
    tb_default_not_implemented("platform.d3dcompiler", "tb_d3dcompiler_release");
}

const char *tb_d3dcompiler_version(const tb_d3dcompiler *compiler)
{
    (void)compiler;
    tb_default_not_implemented("platform.d3dcompiler", "tb_d3dcompiler_version");
}
