/* Compiling for D3D11 through D3DCompiler: the HLSL written from the form
 * (shader_hlsl.h), compiled for a stage's profile of Shader Model 5, and
 * D3DCompiler's messages read back as the compiler's own diagnostics. */
#ifndef TB_SHADER_DXBC_H
#define TB_SHADER_DXBC_H

#include "shader_hlsl.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/d3dcompiler.h"
#include "badger/shader.h"

#include <stddef.h>

/* The shader model DXBC is written for, the only one D3D11 runs. */
#define TB_SHADER_DXBC_MODEL TB_SHADER_MODEL(5, 0)

/* A compile request's shader model for its target, its default applied:
 * 6.6 for DXIL and the reflection alone, 5.0 for DXBC. */
unsigned tb_shader_target_model(const tb_shader_compile_request *request);

/* The name of the compiler a request's target is compiled by, as it names
 * itself, which the shader cache keys on; NULL when the request has none. */
const char *tb_shader_compiler_name(const tb_shader_compile_request *request);

/* Bytes a profile of Shader Model 5 needs, its NUL included: `vs_5_0`. */
#define TB_SHADER_DXBC_PROFILE_CAPACITY ((size_t)8)

/* Write the profile D3DCompiler compiles a stage for, into `buffer` of
 * TB_SHADER_DXBC_PROFILE_CAPACITY bytes: `ps_5_0`. False for a stage Shader
 * Model 5 does not have. */
bool tb_shader_dxbc_profile(tb_shader_stage stage, char *buffer);

/* Read D3DCompiler's messages, appending their errors and warnings to
 * `diagnostics` (of tb_shader_diagnostic), their strings in `arena`. A
 * message placed in `name`, the name the HLSL was given, is placed instead
 * where the line it names was written from, in the shader's own files.
 * Returns the errors appended. */
size_t tb_shader_d3dcompiler_diagnostics(const char *messages, size_t length, const char *name,
                                         const tb_shader_hlsl *hlsl, const char *const *files,
                                         size_t file_count, tb_arena *arena, tb_array *diagnostics);

/* How a DXBC compile reaches D3DCompiler: badger/d3dcompiler.h's
 * functions in the engine, or a test's own. */
typedef struct tb_shader_d3dcompiler_functions
{
    tb_d3dcompiler_result (*compile)(tb_d3dcompiler *compiler,
                                     const tb_d3dcompiler_request *request,
                                     tb_d3dcompiler_output *output, tb_d3dcompiler_error *error);
    void (*release)(tb_d3dcompiler_output *output);
} tb_shader_d3dcompiler_functions;

/* What writing DXBC gave. */
typedef struct tb_shader_dxbc
{
    /* The shader for D3D11 (shader_dxbc_format.h), aligned to 4 bytes;
     * NULL after an error. */
    const void *code;
    size_t code_size;
    size_t error_count; /* Of the diagnostics appended. */
} tb_shader_dxbc;

/* Compile HLSL written for Shader Model 5 with `compiler` for a request
 * whose target was checked, append the compiler's errors and warnings to
 * `diagnostics`, and wrap the DXBC with the HLSL's slots. The code and the
 * diagnostics' strings are in `arena`. Returns false after an error. */
bool tb_shader_write_dxbc(const tb_shader_hlsl *hlsl, const tb_shader_compile_request *request,
                          const char *const *files, size_t file_count,
                          const tb_shader_d3dcompiler_functions *compiler, tb_arena *arena,
                          tb_array *diagnostics, tb_shader_dxbc *dxbc);

#endif
