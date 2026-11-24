/* Compiling shaders into DXIL through DXC, and DXC itself: where it is
 * unavailable it says why, and where it is available every shader of the
 * reflection corpus compiles into a signed DXIL container, which is what
 * D3D12 takes. Cases that need DXC skip where it is unavailable: on Linux
 * and macOS always, and on Windows without the Windows SDK. */
#include "badger/arena.h"
#include "badger/dxc.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdint.h>
#include <string.h>

extern const tb_test_file tb_shader_reflection_files[];
extern const size_t tb_shader_reflection_files_count;

#define MAIN_PATH "/Engine/Private/Main.hlsl"
#define INCLUDE_PATH "/Engine/Public/Counters.hlsli"

/* A DXBC container's header: its four-character code, its hash, its
 * version, its size and its part count, then an offset for each part. */
#define CONTAINER_HASH_OFFSET ((size_t)4)
#define CONTAINER_HASH_SIZE ((size_t)16)
#define CONTAINER_SIZE_OFFSET ((size_t)24)
#define CONTAINER_PART_COUNT_OFFSET ((size_t)28)
#define CONTAINER_HEADER_SIZE ((size_t)32)
/* A part's header: its four-character code and its size. */
#define PART_HEADER_SIZE ((size_t)8)

/* Open DXC, or skip the case, saying why it is unavailable. */
static tb_dxc *open_or_skip(void)
{
    tb_dxc *dxc = NULL;
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    if (tb_dxc_open(&dxc, &error) != TB_DXC_OK)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    return dxc;
}

static uint32_t read_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* Whether a DXBC container whose header and offsets were checked has a part. */
static bool has_part(const unsigned char *code, size_t size, const char *name)
{
    const size_t count = read_u32(code + CONTAINER_PART_COUNT_OFFSET);
    for (size_t index = 0; index < count; index++)
    {
        const size_t offset = read_u32(code + CONTAINER_HEADER_SIZE + index * 4);
        if (offset + PART_HEADER_SIZE <= size && memcmp(code + offset, name, 4) == 0)
        {
            return true;
        }
    }
    return false;
}

/* Check what D3D12 needs of DXIL: a DXBC container of the size it says,
 * whose parts lie within it, holding the program, its input and output
 * signatures and its hash, and signed, which a hash that is not zero shows. */
static void expect_signed_dxil(const tb_shader_compiled *compiled)
{
    const unsigned char *const code = compiled->code;
    const size_t size = compiled->code_size;
    TB_TEST_REQUIRE_POINTER(code, !=, NULL);
    TB_TEST_REQUIRE_UINT(size, >, CONTAINER_HEADER_SIZE);
    TB_TEST_EXPECT_UINT(((uintptr_t)code) % 4, ==, 0);
    TB_TEST_REQUIRE_BYTES(code, "DXBC", 4);
    TB_TEST_EXPECT_UINT(read_u32(code + CONTAINER_SIZE_OFFSET), ==, size);
    const size_t count = read_u32(code + CONTAINER_PART_COUNT_OFFSET);
    TB_TEST_REQUIRE_UINT(CONTAINER_HEADER_SIZE + count * 4, <=, size);
    for (size_t index = 0; index < count; index++)
    {
        const size_t offset = read_u32(code + CONTAINER_HEADER_SIZE + index * 4);
        TB_TEST_REQUIRE_UINT(offset + PART_HEADER_SIZE, <=, size);
        TB_TEST_EXPECT_UINT(offset + PART_HEADER_SIZE + read_u32(code + offset + 4), <=, size);
    }
    TB_TEST_EXPECT_UINT(has_part(code, size, "DXIL"), ==, true);
    TB_TEST_EXPECT_UINT(has_part(code, size, "ISG1"), ==, true);
    TB_TEST_EXPECT_UINT(has_part(code, size, "OSG1"), ==, true);
    TB_TEST_EXPECT_UINT(has_part(code, size, "HASH"), ==, true);
    bool is_signed = false;
    for (size_t index = 0; index < CONTAINER_HASH_SIZE; index++)
    {
        is_signed = is_signed || code[CONTAINER_HASH_OFFSET + index] != 0;
    }
    TB_TEST_EXPECT_UINT(is_signed, ==, true);
}

typedef struct compiled
{
    tb_arena *arena;
    tb_shader_result result;
    tb_shader_compiled output;
} compiled;

static void compile_files(compiled *out, const tb_shader_file *files, size_t file_count,
                          const tb_shader_compile_request *base)
{
    tb_shader_compile_request request = *base;
    request.source.path = MAIN_PATH;
    request.source.files = files;
    request.source.file_count = file_count;
    out->arena = tb_arena_create(0);
    out->result = tb_shader_compile(&request, out->arena, &out->output);
    if (out->output.diagnostic_count > 0)
    {
        TB_TEST_CONTEXT("first diagnostic: %s", out->output.diagnostics[0].message);
    }
}

static void compile_text(compiled *out, const char *text, const tb_shader_compile_request *base)
{
    tb_shader_file file = {0};
    file.path = MAIN_PATH;
    file.text = text;
    file.length = tb_str_length(text);
    compile_files(out, &file, 1, base);
}

/* `// dxc: -T ps_6_6 -E main`: the stage and the entry point a shader is compiled for. */
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

static const tb_shader_diagnostic *find_diagnostic(const tb_shader_compiled *output,
                                                   tb_shader_severity severity, const char *part)
{
    for (size_t index = 0; index < output->diagnostic_count; index++)
    {
        const tb_shader_diagnostic *const diagnostic = &output->diagnostics[index];
        if (diagnostic->severity == severity &&
            tb_str_find(diagnostic->message, 0, TB_STR_END, part) != TB_STR_NOT_FOUND)
        {
            return diagnostic;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * DXC itself.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_dxil, dxc_opens_or_says_why_it_is_unavailable)
{
    tb_dxc *dxc = NULL;
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    const tb_dxc_result result = tb_dxc_open(&dxc, &error);
    TB_TEST_EXPECT_UINT(error.result, ==, result);
    if (result == TB_DXC_OK)
    {
        TB_TEST_EXPECT_POINTER(dxc, !=, NULL);
        TB_TEST_EXPECT_STRING(error.message, ==, "");
    }
    else
    {
        /* Unavailable is a capability the machine lacks, said with a reason. */
        TB_TEST_EXPECT_UINT(result, ==, TB_DXC_UNAVAILABLE);
        TB_TEST_EXPECT_POINTER(dxc, ==, NULL);
        TB_TEST_EXPECT_UINT(tb_str_length(error.message), >, 0);
    }
    tb_dxc_close(dxc);

    /* Without an error to fill, the result alone. */
    TB_TEST_EXPECT_UINT(tb_dxc_open(&dxc, NULL), ==, result);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, nothing_is_closed_or_released_twice)
{
    tb_dxc_close(NULL);
    tb_dxc_output output = {0};
    tb_dxc_release(&output);
    TB_TEST_EXPECT_POINTER(output.code, ==, NULL);
}

TB_TEST(shader_dxil, results_have_names)
{
    TB_TEST_EXPECT_STRING(tb_dxc_result_name(TB_DXC_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_dxc_result_name(TB_DXC_INVALID), ==, "invalid");
    TB_TEST_EXPECT_STRING(tb_dxc_result_name(TB_DXC_UNAVAILABLE), ==, "unavailable");
    TB_TEST_EXPECT_STRING(tb_dxc_result_name(TB_DXC_FAILED), ==, "failed");
    TB_TEST_EXPECT_STRING(tb_dxc_result_name(TB_DXC_NO_MEMORY), ==, "no memory");
    TB_TEST_EXPECT_STRING(tb_dxc_result_name((tb_dxc_result)99), ==, "unknown result");
}

TB_TEST(shader_dxil, dxc_refuses_a_request_that_is_malformed)
{
    tb_dxc *const dxc = open_or_skip();
    tb_dxc_request request = {0};
    tb_dxc_output output = {0};
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a DXC compile names its source and its profile");

    request.name = "/main.hlsl";
    request.profile = "ps_6_6";
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          "a DXC compile's name '/main.hlsl' would be read as an option");

    request.name = "main.hlsl";
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a DXC compile for ps_6_6 names its entry point");

    request.entry_point = "main";
    request.source_length = 4;
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, NULL), ==, TB_DXC_INVALID);

    /* A name DXC takes in UTF-16 must be UTF-8 to convert. */
    request.source = "";
    request.source_length = 0;
    request.entry_point = "\xFF";
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a DXC compile's names are not UTF-8");
    TB_TEST_EXPECT_POINTER(output.compiled, ==, NULL);
    tb_dxc_release(&output);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, dxc_compiles_hlsl_and_reports_its_errors)
{
    tb_dxc *const dxc = open_or_skip();
    static const char source[] = "float4 main() : SV_Target { return 1; }";
    tb_dxc_request request = {0};
    request.source = source;
    request.source_length = sizeof(source) - 1;
    request.name = "main.hlsl";
    request.profile = "ps_6_6";
    request.entry_point = "main";
    tb_dxc_output output = {0};
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, NULL), ==, TB_DXC_OK);
    TB_TEST_EXPECT_UINT(output.code_size, >, CONTAINER_HEADER_SIZE);
    TB_TEST_EXPECT_STRING(output.messages, ==, "");
    tb_dxc_release(&output);
    TB_TEST_EXPECT_POINTER(output.code, ==, NULL);

    /* A library profile compiles with no entry point. */
    static const char library[] = "struct Payload { float4 color; };\n"
                                  "[shader(\"miss\")] void miss_main(inout Payload payload)\n"
                                  "{ payload.color = 0; }\n";
    request.source = library;
    request.source_length = sizeof(library) - 1;
    request.profile = "lib_6_6";
    request.entry_point = NULL;
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, NULL), ==, TB_DXC_OK);
    TB_TEST_EXPECT_UINT(output.code_size, >, CONTAINER_HEADER_SIZE);
    tb_dxc_release(&output);

    static const char broken[] = "float4 main() : SV_Target { return missing; }";
    request.source = broken;
    request.source_length = sizeof(broken) - 1;
    request.profile = "ps_6_6";
    request.entry_point = "main";
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_FAILED);
    TB_TEST_EXPECT_UINT(tb_str_starts_with(error.message, "main.hlsl:1:36: error: use of "
                                                          "undeclared identifier 'missing'"),
                        ==, true);
    TB_TEST_EXPECT_POINTER(output.code, ==, NULL);
    TB_TEST_EXPECT_UINT(tb_str_starts_with(output.messages, error.message), ==, true);
    tb_dxc_release(&output);

    /* The validator writes a summary first; the error is the first that names a place. */
    static const char invalid[] =
        "RWStructuredBuffer<uint64_t> counters;\n"
        "[numthreads(1, 1, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
        "{ InterlockedAdd(counters[id.x], 1); }\n";
    request.source = invalid;
    request.source_length = sizeof(invalid) - 1;
    request.profile = "cs_6_0";
    TB_TEST_EXPECT_UINT(tb_dxc_compile(dxc, &request, &output, &error), ==, TB_DXC_FAILED);
    TB_TEST_EXPECT_UINT(tb_str_starts_with(output.messages, "error: validation errors"), ==, true);
    TB_TEST_EXPECT_UINT(tb_str_starts_with(error.message, "main.hlsl:3:"), ==, true);
    tb_dxc_release(&output);
    tb_dxc_close(dxc);
}

/* -------------------------------------------------------------------------
 * Compiling shaders into DXIL.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_dxil, every_corpus_shader_compiles_into_signed_dxil)
{
    tb_dxc *const dxc = open_or_skip();
    size_t shaders = 0;
    for (size_t index = 0; index < tb_shader_reflection_files_count; index++)
    {
        const tb_test_file *const shader = &tb_shader_reflection_files[index];
        if (!tb_str_ends_with(shader->name, ".hlsl"))
        {
            continue;
        }
        shaders++;
        TB_TEST_CONTEXT("shader %s", shader->name);
        tb_shader_compile_request request = {0};
        char entry[64] = {0};
        TB_TEST_REQUIRE_UINT(
            read_arguments((const char *)shader->bytes, &request.stage, entry, sizeof(entry)), ==,
            true);
        request.entry_point = entry;
        request.target = TB_SHADER_TARGET_DXIL;
        request.dxc = dxc;
        compiled out = {0};
        compile_text(&out, (const char *)shader->bytes, &request);
        TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
        expect_signed_dxil(&out.output);
        tb_arena_destroy(out.arena);
    }
    TB_TEST_EXPECT_UINT(shaders, >, 0);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, a_debug_compile_embeds_its_debug_information)
{
    tb_dxc *const dxc = open_or_skip();
    tb_shader_compile_request request = {0};
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXIL;
    request.dxc = dxc;
    static const char source[] =
        "float4 main(float4 color : COLOR) : SV_Target { return color * 2; }\n";
    compiled optimized = {0};
    compile_text(&optimized, source, &request);
    TB_TEST_REQUIRE_UINT(optimized.result, ==, TB_SHADER_OK);
    expect_signed_dxil(&optimized.output);
    TB_TEST_EXPECT_UINT(has_part(optimized.output.code, optimized.output.code_size, "ILDB"), ==,
                        false);

    request.flags = TB_SHADER_COMPILE_DEBUG;
    compiled debug = {0};
    compile_text(&debug, source, &request);
    TB_TEST_REQUIRE_UINT(debug.result, ==, TB_SHADER_OK);
    expect_signed_dxil(&debug.output);
    /* ILDB is the program with its debug information, which PIX reads. */
    TB_TEST_EXPECT_UINT(has_part(debug.output.code, debug.output.code_size, "ILDB"), ==, true);
    tb_arena_destroy(debug.arena);
    tb_arena_destroy(optimized.arena);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, reflection_alone_writes_no_code)
{
    tb_shader_compile_request request = {0};
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    compiled out = {0};
    compile_text(&out, "float4 main() : SV_Target { return 1; }", &request);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    TB_TEST_EXPECT_POINTER(out.output.code, ==, NULL);
    TB_TEST_EXPECT_UINT(out.output.code_size, ==, 0);
    tb_arena_destroy(out.arena);
}

TB_TEST(shader_dxil, a_target_the_request_cannot_have_is_invalid)
{
    static const struct
    {
        tb_shader_target target;
        unsigned shader_model;
        bool with_dxc;
        const char *message;
    } cases[] = {
        {(tb_shader_target)7, 0, false, "a compile's target is one of tb_shader_target"},
        {TB_SHADER_TARGET_DXIL, 0, false, "a compile for DXIL needs DXC"},
        {TB_SHADER_TARGET_DXIL, TB_SHADER_MODEL(5, 1), true, "DXIL is written for shader model 6"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("case %zu", index);
        tb_shader_compile_request request = {0};
        request.entry_point = "main";
        request.stage = TB_SHADER_STAGE_PIXEL;
        request.target = cases[index].target;
        request.shader_model = cases[index].shader_model;
        /* Never called: the request is refused before DXC is. */
        request.dxc = cases[index].with_dxc ? (tb_dxc *)&request : NULL;
        compiled out = {0};
        compile_text(&out, "float4 main() : SV_Target { return 1; }", &request);
        TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_INVALID);
        TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, ==, 1);
        TB_TEST_EXPECT_STRING(out.output.diagnostics[0].message, ==, cases[index].message);
        TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, "");
        tb_arena_destroy(out.arena);
    }
}

TB_TEST(shader_dxil, dxc_errors_are_placed_in_the_shaders_own_files)
{
    tb_dxc *const dxc = open_or_skip();
    /* 64-bit atomics came with shader model 6.6: in 6.0, DXC's validator
     * refuses the one in the included file, at the line and column of its
     * call there, though DXC compiled one text with the two files in it. */
    static const char main_text[] = "#include \"/Engine/Public/Counters.hlsli\"\n"
                                    "\n"
                                    "[numthreads(1, 1, 1)]\n"
                                    "void main(uint3 id : SV_DispatchThreadID)\n"
                                    "{\n"
                                    "    bump(id.x);\n"
                                    "}\n";
    static const char include_text[] = "RWStructuredBuffer<uint64_t> counters;\n"
                                       "\n"
                                       "void bump(uint index)\n"
                                       "{\n"
                                       "    InterlockedAdd(counters[index], 1);\n"
                                       "}\n";
    tb_shader_file files[2] = {{0}};
    files[0].path = MAIN_PATH;
    files[0].text = main_text;
    files[0].length = sizeof(main_text) - 1;
    files[1].path = INCLUDE_PATH;
    files[1].text = include_text;
    files[1].length = sizeof(include_text) - 1;
    tb_shader_compile_request request = {0};
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_COMPUTE;
    request.target = TB_SHADER_TARGET_DXIL;
    request.shader_model = TB_SHADER_MODEL(6, 0);
    request.dxc = dxc;
    compiled out = {0};
    compile_files(&out, files, 2, &request);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_EXPECT_POINTER(out.output.code, ==, NULL);
    const tb_shader_diagnostic *const atomic =
        find_diagnostic(&out.output, TB_SHADER_ERROR, "64-bit atomic operations");
    TB_TEST_REQUIRE_POINTER(atomic, !=, NULL);
    TB_TEST_EXPECT_STRING(atomic->path, ==, INCLUDE_PATH);
    TB_TEST_EXPECT_UINT(atomic->line, ==, 5);
    TB_TEST_EXPECT_UINT(atomic->column, ==, 5);
    TB_TEST_EXPECT_STRING(atomic->message, ==,
                          "opcode '64-bit atomic operations' should only be used in 'Shader "
                          "Model 6.6+'");
    /* The summary DXC writes before its validator's errors is left out. */
    TB_TEST_EXPECT_POINTER(find_diagnostic(&out.output, TB_SHADER_ERROR, "validation errors"), ==,
                           NULL);
    tb_arena_destroy(out.arena);

    /* With the shader model it needs, it compiles. */
    request.shader_model = 0;
    compile_files(&out, files, 2, &request);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    expect_signed_dxil(&out.output);
    tb_arena_destroy(out.arena);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, a_shader_model_dxc_does_not_know_is_its_error)
{
    tb_dxc *const dxc = open_or_skip();
    tb_shader_compile_request request = {0};
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXIL;
    request.shader_model = TB_SHADER_MODEL(6, 99);
    request.dxc = dxc;
    compiled out = {0};
    compile_text(&out, "float4 main() : SV_Target { return 1; }", &request);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(out.output.diagnostic_count, >, 0);
    TB_TEST_EXPECT_UINT(out.output.diagnostics[0].severity, ==, TB_SHADER_ERROR);
    TB_TEST_EXPECT_STRING(out.output.diagnostics[0].path, ==, "");
    tb_arena_destroy(out.arena);
    tb_dxc_close(dxc);
}

TB_TEST(shader_dxil, dxc_warnings_are_kept_with_the_code)
{
    tb_dxc *const dxc = open_or_skip();
    tb_shader_compile_request request = {0};
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXIL;
    request.dxc = dxc;
    compiled out = {0};
    compile_text(&out,
                 "float4 main(float4 p : SV_Position) : SV_Target\n"
                 "{\n"
                 "    float4 v = p;\n"
                 "    if (v.x = 1) { v.y = 2; }\n"
                 "    return v;\n"
                 "}\n",
                 &request);
    TB_TEST_EXPECT_UINT(out.result, ==, TB_SHADER_OK);
    expect_signed_dxil(&out.output);
    const tb_shader_diagnostic *const warning =
        find_diagnostic(&out.output, TB_SHADER_WARNING, "assignment as a condition");
    TB_TEST_REQUIRE_POINTER(warning, !=, NULL);
    TB_TEST_EXPECT_STRING(warning->path, ==, MAIN_PATH);
    TB_TEST_EXPECT_UINT(warning->line, ==, 4);
    /* At the `=`, as DXC places it, though DXC read the line without its indent. */
    TB_TEST_EXPECT_UINT(warning->column, ==, 13);
    tb_arena_destroy(out.arena);
    tb_dxc_close(dxc);
}
