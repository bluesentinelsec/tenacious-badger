/* Compiling for D3D12 through DXC: the profile a stage compiles for, and
 * DXC's messages read back as the compiler's own diagnostics. */
#ifndef TB_SHADER_DXIL_H
#define TB_SHADER_DXIL_H

#include "shader_internal.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/dxc.h"
#include "badger/shader.h"

#include <stddef.h>

/* A compile request's shader model, its default applied. */
unsigned tb_shader_dxil_model(const tb_shader_compile_request *request);

/* Bytes a profile needs, its NUL included: `lib_255_255`. */
#define TB_SHADER_PROFILE_CAPACITY ((size_t)16)

/* Write the profile DXC compiles a stage for in a shader model, as `-T`
 * takes it, into `buffer` of TB_SHADER_PROFILE_CAPACITY bytes: `ps_6_6`, and
 * for a ray-tracing stage, a library's, `lib_6_6`. */
void tb_shader_dxil_profile(tb_shader_stage stage, unsigned shader_model, char *buffer);

/* The text DXC compiled, and where each of its unit's tokens starts in it. */
typedef struct tb_shader_rendered
{
    const tb_shader_unit *unit;
    const char *text;
    size_t length;
    const size_t *offsets; /* One for each of the unit's tokens, ascending. */
} tb_shader_rendered;

/* Read DXC's messages, appending their errors and warnings to `diagnostics`
 * (of tb_shader_diagnostic), their strings in `arena`. A message DXC places
 * in `name`, the name it gave the rendered text, is placed instead where the
 * token at that line and column came from, in the shader's own files.
 * DXC's notes, its excerpts of the source, and its summary of validation
 * errors, which the errors themselves follow, are left out. Returns the
 * errors appended. */
size_t tb_shader_dxc_diagnostics(const char *messages, size_t length, const char *name,
                                 const tb_shader_rendered *rendered, tb_arena *arena,
                                 tb_array *diagnostics);

/* How a DXIL compile reaches DXC: badger/dxc.h's tb_dxc_compile() and
 * tb_dxc_release() in the engine, or a test's own, which stand in for DXC
 * where there is none. */
typedef struct tb_shader_dxc_functions
{
    tb_dxc_result (*compile)(tb_dxc *dxc, const tb_dxc_request *request, tb_dxc_output *output,
                             tb_dxc_error *error);
    void (*release)(tb_dxc_output *output);
} tb_shader_dxc_functions;

/* What writing DXIL gave. */
typedef struct tb_shader_dxil
{
    const void *code; /* The DXIL container, aligned to 4 bytes; NULL after an error. */
    size_t code_size;
    size_t error_count; /* Of the diagnostics appended. */
} tb_shader_dxil;

/* Compile a unit, one that checked, into DXIL for a request whose target was
 * checked: render its text without `#line`, so that each place DXC names is
 * a place in the text, which the tokens' offsets lead back to the sources;
 * compile that with `dxc`, passed the request's DXC; and append DXC's errors
 * and warnings to `diagnostics` (of tb_shader_diagnostic). The code and the
 * diagnostics' strings are in `arena`. Returns false after an error. */
bool tb_shader_write_dxil(const tb_shader_unit *unit, const tb_shader_compile_request *request,
                          const tb_shader_dxc_functions *dxc, tb_arena *arena,
                          tb_array *diagnostics, tb_shader_dxil *dxil);

#endif
