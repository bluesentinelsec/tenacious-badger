/* Compiling for D3D12 through DXC: the profile a stage compiles for, and
 * DXC's messages read back as the compiler's own diagnostics. */
#ifndef TB_SHADER_DXIL_H
#define TB_SHADER_DXIL_H

#include "shader_internal.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/shader.h"

#include <stddef.h>

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

#endif
