/* Compiling shaders into DXBC for D3D11 through D3DCompiler, and
 * D3DCompiler itself: where it is unavailable it says why, and where it is
 * available every shader of the reflection corpus that Shader Model 5 can
 * run compiles into a DXBC container, and every other one is refused with
 * a diagnostic that says what Shader Model 5 lacks. Cases that need
 * D3DCompiler skip where it is unavailable: on Linux and macOS always. */
#include "badger/arena.h"
#include "badger/d3dcompiler.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdint.h>
#include <string.h>

extern const tb_test_file tb_shader_reflection_files[];
extern const size_t tb_shader_reflection_files_count;

#define MAIN_PATH "/Engine/Private/Main.hlsl"

/* The engine's table before the DXBC: its size with no slots, and where it
 * says the DXBC is (shader_dxbc_format.h). */
#define TABLE_HEADER_SIZE ((size_t)32)
#define TABLE_SLOT_SIZE ((size_t)12)
#define TABLE_CODE_OFFSET ((size_t)24)
#define TABLE_SLOT_COUNT_OFFSET ((size_t)8)
/* A DXBC container's size, after its code and its hash. */
#define CONTAINER_SIZE_OFFSET ((size_t)24)

/* What Shader Model 5 cannot run of the corpus, and what the error that
 * refuses it says: the compiler's own, or D3DCompiler's. */
static const struct
{
    const char *name;
    const char *why;
} refused[] = {
    {"buffer_layout.hlsl", "64-bit floats"},
    {"compute_features.hlsl", "Shader Model 5"},
    {"heap_buffers.hlsl", "Buffer<float4> from a descriptor heap"},
    {"heap_compute.hlsl", "StructuredBuffer<float> from a descriptor heap"},
    {"heap_constant_buffer.hlsl", "constant buffer from a descriptor heap"},
    {"heap_textures.hlsl", "index the shader computes"},
    {"mesh_outputs.hlsl", "the mesh stage"},
    {"pixel_bindings.hlsl", "register space other than 0"},
    {"signature_arrays_share_rows.hlsl", "SV_Barycentrics"},
    {"signature_modes_and_shading_rate.hlsl", "SV_ShadingRate"},
};

static tb_d3dcompiler *open_or_skip(void)
{
    tb_d3dcompiler *compiler = NULL;
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    if (tb_d3dcompiler_open(&compiler, &error) != TB_D3DCOMPILER_OK)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    return compiler;
}

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* Check what the D3D11 backend takes: the engine's table, then a DXBC
 * container of the size it says, which ends the code. */
static void expect_dxbc(const tb_shader_compiled *compiled)
{
    const unsigned char *const code = compiled->code;
    const size_t size = compiled->code_size;
    TB_TEST_REQUIRE_POINTER(code, !=, NULL);
    TB_TEST_REQUIRE_UINT(size, >, TABLE_HEADER_SIZE);
    TB_TEST_EXPECT_UINT(((uintptr_t)code) % 4, ==, 0);
    TB_TEST_REQUIRE_BYTES(code, "TB11", 4);
    const size_t slots = read_u32(code + TABLE_SLOT_COUNT_OFFSET);
    const size_t start = read_u32(code + TABLE_CODE_OFFSET);
    TB_TEST_REQUIRE_UINT(start, ==, TABLE_HEADER_SIZE + slots * TABLE_SLOT_SIZE);
    TB_TEST_REQUIRE_UINT(start + CONTAINER_SIZE_OFFSET + 4, <=, size);
    TB_TEST_REQUIRE_BYTES(code + start, "DXBC", 4);
    TB_TEST_EXPECT_UINT(read_u32(code + start + CONTAINER_SIZE_OFFSET), ==, size - start);
}

typedef struct compiled
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_compiled output;
} compiled;

static void compile_text(compiled *out, const char *text, size_t length, const char *entry,
                         tb_shader_stage stage, tb_d3dcompiler *compiler)
{
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = text;
    file.length = length;
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = entry;
    request.stage = stage;
    request.target = TB_SHADER_TARGET_DXBC;
    request.d3dcompiler = compiler;
    out->arena = tb_arena_create(0);
    out->result = tb_shader_compile(&request, out->arena, &out->output);
    if (out->output.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("first diagnostic: %s", out->output.diagnostics[0].message);
    }
}

static const tb_shader_diagnostic *find_error(const tb_shader_compiled *output, const char *part)
{
    for (size_t index = 0; index < output->diagnostic_count; index++)
    {
        const tb_shader_diagnostic *const diagnostic = &output->diagnostics[index];
        if (diagnostic->severity == TB_SHADER_ERROR &&
            tb_str_find(diagnostic->message, 0, TB_STR_END, part) != TB_STR_NOT_FOUND)
        {
            return diagnostic;
        }
    }
    return NULL;
}

/* `// dxc: -T ps_6_6 -E main`: the stage and the entry point of a corpus shader. */
static bool read_arguments(const char *text, tb_shader_stage *stage, char *entry, size_t size)
{
    static const struct
    {
        const char *prefix;
        tb_shader_stage stage;
    } profiles[] = {
        {"vs_", TB_SHADER_STAGE_VERTEX},        {"ps_", TB_SHADER_STAGE_PIXEL},
        {"gs_", TB_SHADER_STAGE_GEOMETRY},      {"hs_", TB_SHADER_STAGE_HULL},
        {"ds_", TB_SHADER_STAGE_DOMAIN},        {"cs_", TB_SHADER_STAGE_COMPUTE},
        {"as_", TB_SHADER_STAGE_AMPLIFICATION}, {"ms_", TB_SHADER_STAGE_MESH},
    };
    const size_t profile = tb_str_find(text, 0, tb_str_length(text), "-T ");
    const size_t name = tb_str_find(text, 0, tb_str_length(text), "-E ");
    const size_t line_end = tb_str_find_char(text, 0, tb_str_length(text), '\n');
    if (profile >= line_end || name >= line_end)
    {
        return false;
    }
    bool found = false;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(profiles); index++)
    {
        if (tb_str_matches_at(text, profile + 3, profiles[index].prefix))
        {
            *stage = profiles[index].stage;
            found = true;
        }
    }
    const size_t start = name + 3;
    size_t end = start;
    while (end < line_end && text[end] != ' ' && text[end] != '\r' && end - start + 1 < size)
    {
        end++;
    }
    memcpy(entry, text + start, end - start);
    entry[end - start] = '\0';
    return found;
}

/* -------------------------------------------------------------------------
 * D3DCompiler itself.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_dxbc, d3dcompiler_opens_or_says_why_it_is_unavailable)
{
    tb_d3dcompiler *compiler = NULL;
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    const tb_d3dcompiler_result result = tb_d3dcompiler_open(&compiler, &error);
    TB_TEST_EXPECT_UINT(error.result, ==, result);
    if (result == TB_D3DCOMPILER_OK)
    {
        TB_TEST_EXPECT_POINTER(compiler, !=, NULL);
        TB_TEST_EXPECT_STRING(error.message, ==, "");
    }
    else
    {
        /* Unavailable is a capability the machine lacks, said with a reason. */
        TB_TEST_EXPECT_UINT(result, ==, TB_D3DCOMPILER_UNAVAILABLE);
        TB_TEST_EXPECT_POINTER(compiler, ==, NULL);
        TB_TEST_EXPECT_UINT(tb_str_length(error.message), >, 0);
    }
    tb_d3dcompiler_close(compiler);

    /* Without an error to fill, the result alone. */
    TB_TEST_EXPECT_UINT(tb_d3dcompiler_open(&compiler, NULL), ==, result);
    tb_d3dcompiler_close(compiler);
}

TB_TEST(shader_dxbc, d3dcompiler_names_its_version)
{
    /* The shader cache keys on this name, so it must tell apart the
     * compilers that compile differently: its file's version. */
    tb_d3dcompiler *const compiler = open_or_skip();
    const char *const version = tb_d3dcompiler_version(compiler);
    TB_TEST_EXPECT(tb_str_starts_with(version, "d3dcompiler_47 10."));
    TB_TEST_EXPECT_UINT(tb_str_length(version), <, TB_D3DCOMPILER_VERSION_CAPACITY);
    tb_d3dcompiler *const again = open_or_skip();
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_version(again), ==, version);
    tb_d3dcompiler_close(again);
    tb_d3dcompiler_close(compiler);
}

TB_TEST(shader_dxbc, nothing_is_closed_or_released_twice)
{
    tb_d3dcompiler_close(NULL);
    tb_d3dcompiler_output output = {0};
    tb_d3dcompiler_release(&output);
    TB_TEST_EXPECT_POINTER(output.code, ==, NULL);
}

TB_TEST(shader_dxbc, results_have_names)
{
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name(TB_D3DCOMPILER_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name(TB_D3DCOMPILER_INVALID), ==, "invalid");
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name(TB_D3DCOMPILER_UNAVAILABLE), ==,
                          "unavailable");
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name(TB_D3DCOMPILER_FAILED), ==, "failed");
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name(TB_D3DCOMPILER_NO_MEMORY), ==, "no memory");
    TB_TEST_EXPECT_STRING(tb_d3dcompiler_result_name((tb_d3dcompiler_result)99), ==,
                          "unknown result");
}

TB_TEST(shader_dxbc, d3dcompiler_refuses_a_request_that_is_malformed)
{
    tb_d3dcompiler *const compiler = open_or_skip();
    tb_d3dcompiler_request request = {0};
    tb_d3dcompiler_output output = {0};
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    TB_TEST_EXPECT_UINT(tb_d3dcompiler_compile(compiler, &request, &output, &error), ==,
                        TB_D3DCOMPILER_INVALID);
    TB_TEST_EXPECT_STRING(
        error.message, ==,
        "a D3DCompiler compile names its source, its profile and its entry point");
    request.name = "main.hlsl";
    request.profile = "ps_5_0";
    request.entry_point = "main";
    request.source_length = 4;
    TB_TEST_EXPECT_UINT(tb_d3dcompiler_compile(compiler, &request, &output, &error), ==,
                        TB_D3DCOMPILER_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a D3DCompiler compile of 4 bytes has no source");
    tb_d3dcompiler_release(&output);
    tb_d3dcompiler_close(compiler);
}

TB_TEST(shader_dxbc, d3dcompiler_compiles_hlsl_or_says_what_is_wrong)
{
    static const char good[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
    static const char bad[] = "float4 main() : SV_Target {\n    return missing;\n}";
    tb_d3dcompiler *const compiler = open_or_skip();
    tb_d3dcompiler_request request = {0};
    tb_d3dcompiler_output output = {0};
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    request.name = "main.hlsl";
    request.profile = "ps_5_0";
    request.entry_point = "main";
    request.source = good;
    request.source_length = sizeof(good) - 1;
    TB_TEST_REQUIRE_UINT(tb_d3dcompiler_compile(compiler, &request, &output, &error), ==,
                         TB_D3DCOMPILER_OK);
    TB_TEST_REQUIRE_UINT(output.code_size, >, 4);
    TB_TEST_EXPECT_BYTES(output.code, "DXBC", 4);
    tb_d3dcompiler_release(&output);

    /* Debug information makes other bytes, which still run. */
    request.flags = TB_D3DCOMPILER_DEBUG;
    TB_TEST_REQUIRE_UINT(tb_d3dcompiler_compile(compiler, &request, &output, &error), ==,
                         TB_D3DCOMPILER_OK);
    TB_TEST_EXPECT_BYTES(output.code, "DXBC", 4);
    tb_d3dcompiler_release(&output);

    request.flags = 0;
    request.source = bad;
    request.source_length = sizeof(bad) - 1;
    TB_TEST_EXPECT_UINT(tb_d3dcompiler_compile(compiler, &request, &output, &error), ==,
                        TB_D3DCOMPILER_FAILED);
    TB_TEST_EXPECT_POINTER(output.code, ==, NULL);
    /* The message names the line, as the compiler writes it: `main.hlsl(2,12-18): error X3004`. */
    TB_TEST_EXPECT_UINT(tb_str_find(output.messages, 0, TB_STR_END, "(2,"), !=, TB_STR_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_str_find(error.message, 0, TB_STR_END, "error X3004"), !=,
                        TB_STR_NOT_FOUND);
    tb_d3dcompiler_release(&output);
    tb_d3dcompiler_close(compiler);
}

/* -------------------------------------------------------------------------
 * Compiling a shader for D3D11.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_dxbc, a_request_without_d3dcompiler_or_for_another_model_is_invalid)
{
    static const char text[] = "float4 main() : SV_Target { return 1; }";
    tb_shader_file file = {MAIN_PATH, text, sizeof(text) - 1};
    tb_shader_compile_request request = {0};
    tb_shader_compiled output = {0};
    tb_arena *const arena = tb_arena_create(0);
    request.source.path = MAIN_PATH;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXBC;
    TB_TEST_EXPECT_INT(tb_shader_compile(&request, arena, &output), ==, TB_SHADER_INVALID);
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(output.diagnostics[0].message, ==,
                          "a compile for DXBC needs D3DCompiler");
    /* Any compiler will do to be refused for the model. */
    request.d3dcompiler = (struct tb_d3dcompiler *)&file;
    request.shader_model = TB_SHADER_MODEL(6, 0);
    TB_TEST_EXPECT_INT(tb_shader_compile(&request, arena, &output), ==, TB_SHADER_INVALID);
    TB_TEST_REQUIRE_UINT(output.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(output.diagnostics[0].message, ==,
                          "DXBC is written for shader model 5.0, which D3D11 runs");
    tb_arena_destroy(arena);
}

TB_TEST(shader_dxbc, the_corpus_compiles_or_says_what_shader_model_5_lacks)
{
    tb_d3dcompiler *const compiler = open_or_skip();
    size_t compiled_count = 0;
    for (size_t index = 0; index < tb_shader_reflection_files_count; index++)
    {
        const tb_test_file *const file = &tb_shader_reflection_files[index];
        if (!tb_str_ends_with(file->name, ".hlsl"))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        const char *const text = (const char *)file->bytes;
        tb_shader_stage stage = TB_SHADER_STAGE_PIXEL;
        char entry[64] = {0};
        TB_TEST_REQUIRE(read_arguments(text, &stage, entry, sizeof(entry)));
        compiled out = {0};
        compile_text(&out, text, file->length, entry, stage, compiler);
        const char *why = NULL;
        for (size_t at = 0; at < TB_TEST_COUNT_OF(refused); at++)
        {
            why = tb_str_equal(refused[at].name, file->name) ? refused[at].why : why;
        }
        if (why == NULL)
        {
            TB_TEST_EXPECT_INT(out.result, ==, TB_SHADER_OK);
            if (out.result == TB_SHADER_OK)
            {
                expect_dxbc(&out.output);
                compiled_count++;
            }
        }
        else
        {
            TB_TEST_EXPECT_INT(out.result, ==, TB_SHADER_FAILED);
            const tb_shader_diagnostic *const error = find_error(&out.output, why);
            TB_TEST_EXPECT_POINTER(error, !=, NULL);
            /* D3DCompiler's own errors are placed in the shader's file: this
             * one, of a parameter, at the function that declares it. */
            if (error != NULL && tb_str_equal(why, "SV_Barycentrics"))
            {
                TB_TEST_EXPECT_STRING(error->path, ==, MAIN_PATH);
                TB_TEST_EXPECT_UINT(error->line, >, 1);
            }
        }
        tb_arena_destroy(out.arena);
    }
    TB_TEST_EXPECT_UINT(compiled_count, ==, 18);
    tb_d3dcompiler_close(compiler);
}
