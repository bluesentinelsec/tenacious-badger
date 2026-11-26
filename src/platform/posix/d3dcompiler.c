/* D3DCompiler on Linux and macOS: unavailable. DXBC runs only on D3D11,
 * which only Windows has, so these systems lack the capability; no work is
 * missing. */
#include "badger/d3dcompiler.h"

#include "badger/fmt.h"

#include <stddef.h>

static tb_d3dcompiler_result unavailable(tb_d3dcompiler_error *error)
{
    if (error != NULL)
    {
        error->result = TB_D3DCOMPILER_UNAVAILABLE;
        (void)tb_fmt_buffer(error->message, sizeof(error->message),
                            "D3DCompiler compiles DXBC for D3D11, which runs only on Windows; this "
                            "system's backends need no DXBC");
    }
    return TB_D3DCOMPILER_UNAVAILABLE;
}

tb_d3dcompiler_result tb_d3dcompiler_open(tb_d3dcompiler **compiler, tb_d3dcompiler_error *error)
{
    *compiler = NULL;
    return unavailable(error);
}

void tb_d3dcompiler_close(tb_d3dcompiler *compiler)
{
    (void)compiler;
}

tb_d3dcompiler_result tb_d3dcompiler_compile(tb_d3dcompiler *compiler,
                                             const tb_d3dcompiler_request *request,
                                             tb_d3dcompiler_output *output,
                                             tb_d3dcompiler_error *error)
{
    /* coverage-off: tb_d3dcompiler_open() opens no compiler here, so nothing can call this */
    const tb_d3dcompiler_output empty = {0};
    (void)compiler;
    (void)request;
    *output = empty;
    return unavailable(error);
    /* coverage-on */
}

const char *tb_d3dcompiler_version(const tb_d3dcompiler *compiler)
{
    /* coverage-off: tb_d3dcompiler_open() opens no compiler here, so nothing can call this */
    (void)compiler;
    return "";
    /* coverage-on */
}

void tb_d3dcompiler_release(tb_d3dcompiler_output *output)
{
    const tb_d3dcompiler_output empty = {0};
    *output = empty;
}
