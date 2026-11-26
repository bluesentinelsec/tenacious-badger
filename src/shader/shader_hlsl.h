/* HLSL written from the compiler's own form, for a compiler that cannot
 * read the shader's source as it is written: D3DCompiler, which knows
 * Shader Model 5 alone.
 *
 * What the form wrote out (implicit conversions, compound assignments,
 * short-circuit operators, `out` arguments, methods of structs) is written
 * as the form has it, so the text is plain HLSL of any version. What Shader
 * Model 5 lacks is written in its terms:
 *
 * - a resource or a sampler taken from a descriptor heap is a register of
 *   its own, which the backend binds before each draw from the heap index
 *   the shader reads from a constant buffer (shader_dxbc_format.h); an
 *   index the shader computes cannot be bound so, and is an error;
 * - `Load<T>` and `Store<T>` of a byte-address buffer are `Load4` and
 *   `Store4` and their kin, reinterpreted;
 * - `select`, `and` and `or` are `?:`, `&&` and `||`, which Shader Model 5
 *   applies component by component;
 * - a constant buffer is a `cbuffer` whose variables are named after it.
 *
 * What Shader Model 5 cannot do at all, such as wave operations, 16-bit and
 * 64-bit integers and the stages of ray tracing and mesh shaders, is an
 * error that says so, placed where the source uses it. */
#ifndef TB_SHADER_HLSL_H
#define TB_SHADER_HLSL_H

#include "shader_dxbc_format.h"
#include "shader_form.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/shader.h"

#include <stddef.h>

/* What to write HLSL from. */
typedef struct tb_shader_hlsl_request
{
    const tb_shader_form *form;             /* One entry point's, extracted. */
    const tb_shader_reflection *reflection; /* Of that form: its registers and buffers. */
    const char *const *files;               /* The paths locations name. */
    size_t file_count;
} tb_shader_hlsl_request;

/* What writing gave. */
typedef struct tb_shader_hlsl
{
    const char *text; /* NUL-terminated; NULL after an error. */
    size_t length;
    /* Where each line of the text came from, the first line first: what a
     * compiler's message about a line is placed at. */
    const tb_shader_location *lines;
    size_t line_count;
    /* The registers given to what the shader takes from the heaps. */
    tb_shader_dxbc_slot slots[TB_SHADER_DXBC_MAX_SLOTS];
    uint32_t slot_count;
    /* Bytes the shader reads of each constant buffer the RHI binds. */
    uint32_t constant_bytes[TB_SHADER_DXBC_CONSTANT_BUFFERS];
    size_t error_count; /* Of the diagnostics appended. */
} tb_shader_hlsl;

/* Write the one entry point of a form as HLSL of Shader Model 5, in
 * `arena`. Errors, each placed in the shader's files where it can be, are
 * appended to `diagnostics` (of tb_shader_diagnostic), their strings in
 * `arena`. Returns false after an error. */
bool tb_shader_write_hlsl(const tb_shader_hlsl_request *request, tb_arena *arena,
                          tb_array *diagnostics, tb_shader_hlsl *hlsl);

#endif
