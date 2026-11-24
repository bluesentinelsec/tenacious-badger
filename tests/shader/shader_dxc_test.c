/* What a DXIL compile asks of DXC, and DXC's messages read back as the
 * compiler's own diagnostics. DXC compiles the text the preprocessor
 * rendered, so the places it names are in that text: each must lead back to
 * the token there, in the shader's own file. These cases stand in for DXC,
 * with messages written as DXC writes them, so they run on every platform,
 * DXC or not; tests/shader/shader_dxil_test.c compiles with DXC itself. */
#include "badger/arena.h"
#include "badger/array.h"
#include "badger/dxc.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/str.h"
#include "shader/shader_dxil.h"
#include "shader/shader_internal.h"
#include "tb_test.h"

#include <string.h>

#define MAIN_PATH "/Engine/Private/Main.hlsl"
#define COMMON_PATH "/Engine/Public/Common.hlsli"

/* What the stand-in for DXC was asked, and what it answers. One case uses
 * it at a time. */
static struct
{
    tb_dxc *dxc;
    char name[64];
    char profile[16];
    char entry_point[16];
    unsigned flags;
    char source[512];
    tb_dxc_result result;
    const char *messages;
    const char *error;
    size_t released;
} fake;

static const unsigned char fake_code[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4};

static tb_dxc_result fake_compile(tb_dxc *dxc, const tb_dxc_request *request, tb_dxc_output *output,
                                  tb_dxc_error *error)
{
    fake.dxc = dxc;
    (void)tb_fmt_buffer(fake.name, sizeof(fake.name), "%s", request->name);
    (void)tb_fmt_buffer(fake.profile, sizeof(fake.profile), "%s", request->profile);
    (void)tb_fmt_buffer(fake.entry_point, sizeof(fake.entry_point), "%s", request->entry_point);
    fake.flags = request->flags;
    (void)tb_fmt_buffer(fake.source, sizeof(fake.source), "%.*s", (int)request->source_length,
                        request->source);
    const tb_dxc_output empty = {0};
    *output = empty;
    output->messages = fake.messages;
    output->messages_length = tb_str_length(fake.messages);
    if (fake.result == TB_DXC_OK)
    {
        output->code = fake_code;
        output->code_size = sizeof(fake_code);
    }
    error->result = fake.result;
    (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s", fake.error);
    return fake.result;
}

static void fake_release(tb_dxc_output *output)
{
    const tb_dxc_output empty = {0};
    *output = empty;
    fake.released++;
}

TB_TEST(shader_dxc, each_stage_has_its_profile)
{
    static const struct
    {
        tb_shader_stage stage;
        unsigned shader_model;
        const char *profile;
    } cases[] = {
        {TB_SHADER_STAGE_VERTEX, TB_SHADER_MODEL(6, 6), "vs_6_6"},
        {TB_SHADER_STAGE_PIXEL, TB_SHADER_MODEL(6, 0), "ps_6_0"},
        {TB_SHADER_STAGE_GEOMETRY, TB_SHADER_MODEL(6, 6), "gs_6_6"},
        {TB_SHADER_STAGE_HULL, TB_SHADER_MODEL(6, 6), "hs_6_6"},
        {TB_SHADER_STAGE_DOMAIN, TB_SHADER_MODEL(6, 6), "ds_6_6"},
        {TB_SHADER_STAGE_COMPUTE, TB_SHADER_MODEL(6, 8), "cs_6_8"},
        {TB_SHADER_STAGE_AMPLIFICATION, TB_SHADER_MODEL(6, 5), "as_6_5"},
        {TB_SHADER_STAGE_MESH, TB_SHADER_MODEL(6, 5), "ms_6_5"},
        /* Ray tracing compiles as a library, every entry point at once. */
        {TB_SHADER_STAGE_RAY_GENERATION, TB_SHADER_MODEL(6, 3), "lib_6_3"},
        {TB_SHADER_STAGE_CLOSEST_HIT, TB_SHADER_MODEL(6, 6), "lib_6_6"},
        {TB_SHADER_STAGE_CALLABLE, TB_SHADER_MODEL(255, 255), "lib_255_255"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        char profile[TB_SHADER_PROFILE_CAPACITY] = {0};
        tb_shader_dxil_profile(cases[index].stage, cases[index].shader_model, profile);
        TB_TEST_EXPECT_STRING(profile, ==, cases[index].profile);
    }
}

typedef struct rendered_source
{
    tb_arena *arena;
    tb_shader_unit unit;
    tb_array *text;
    tb_array *offsets;
    tb_shader_rendered rendered;
    tb_array *diagnostics;
} rendered_source;

/* Preprocess the main file and the one it includes, and render them as a
 * compile for DXC does: without `#line`, each token's offset kept. */
static void render_source(rendered_source *out, const char *main_text, const char *common_text)
{
    out->arena = tb_arena_create(0);
    const tb_allocator allocator = tb_arena_allocator(out->arena);
    tb_shader_file files[2] = {{0}};
    files[0].path = MAIN_PATH;
    files[0].text = main_text;
    files[0].length = tb_str_length(main_text);
    files[1].path = COMMON_PATH;
    files[1].text = common_text;
    files[1].length = tb_str_length(common_text);
    tb_shader_preprocess_request request = {0};
    request.path = MAIN_PATH;
    request.files = files;
    request.file_count = 2;
    TB_TEST_REQUIRE_UINT(tb_shader_preprocess_unit(&request, out->arena, &out->unit), ==,
                         TB_SHADER_OK);
    out->text = TB_ARRAY_CREATE(&allocator, char);
    out->offsets = TB_ARRAY_CREATE(&allocator, size_t);
    tb_shader_render(&out->unit, false, out->text, out->offsets);
    const char terminator = '\0';
    tb_array_push(out->text, &terminator);
    out->rendered.unit = &out->unit;
    out->rendered.text = tb_array_data(out->text);
    out->rendered.length = tb_array_length(out->text) - 1;
    out->rendered.offsets = tb_array_data(out->offsets);
    out->diagnostics = TB_ARRAY_CREATE(&allocator, tb_shader_diagnostic);
}

static size_t read_messages(rendered_source *source, const char *messages)
{
    return tb_shader_dxc_diagnostics(messages, tb_str_length(messages), MAIN_PATH,
                                     &source->rendered, source->arena, source->diagnostics);
}

static void expect_diagnostic(const rendered_source *source, size_t index,
                              tb_shader_severity severity, const char *path, size_t line,
                              size_t column, const char *message)
{
    TB_TEST_CONTEXT("diagnostic %zu", index);
    TB_TEST_REQUIRE_UINT(index, <, tb_array_length(source->diagnostics));
    const tb_shader_diagnostic *const diagnostic =
        TB_ARRAY_AT(source->diagnostics, tb_shader_diagnostic, index);
    TB_TEST_EXPECT_UINT(diagnostic->severity, ==, severity);
    TB_TEST_EXPECT_STRING(diagnostic->path, ==, path);
    TB_TEST_EXPECT_UINT(diagnostic->line, ==, line);
    TB_TEST_EXPECT_UINT(diagnostic->column, ==, column);
    TB_TEST_EXPECT_STRING(diagnostic->message, ==, message);
}

static const char main_text[] = "#include \"/Engine/Public/Common.hlsli\"\n"
                                "float4 main() : SV_Target\n"
                                "{\n"
                                "    return    scale(2);\n"
                                "}\n";
static const char common_text[] = "float4 scale(float x)\n"
                                  "{\n"
                                  "    return x * 0.5;\n"
                                  "}\n";

TB_TEST(shader_dxc, places_in_the_text_lead_back_to_the_sources)
{
    rendered_source source = {0};
    render_source(&source, main_text, common_text);
    /* What DXC reads: the included file first, then the main file, each
     * token on a line of its own file, the space between tokens one space. */
    TB_TEST_REQUIRE_STRING(source.rendered.text, ==,
                           "float4 scale(float x)\n"
                           "{\n"
                           "return x * 0.5;\n"
                           "}\n"
                           "float4 main() : SV_Target\n"
                           "{\n"
                           "return scale(2);\n"
                           "}\n");
    const size_t errors =
        read_messages(&source,
                      /* A column inside a token: its own column, and as far into it. */
                      MAIN_PATH ":7:9: error: no matching function for call to 'scale'\n"
                                "return scale(2);\n"
                                "        ^\n"
                      /* In the included file. */
                      MAIN_PATH ":3:8: warning: implicit truncation of vector type\n"
                      /* A line alone is its first token. */
                      MAIN_PATH ":7: error: Function uses features incompatible with "
                                "the shader model.\r\n"
                      /* A column past the line's end is its last token. */
                      MAIN_PATH ":5:400: error: far along\n"
                      /* A column between tokens is the token before. */
                      MAIN_PATH ":5:14: error: between\n"
                                "note: at '%5 = fmul fast float %1, undef' in block '#0'\n");
    TB_TEST_EXPECT_UINT(errors, ==, 4);
    TB_TEST_REQUIRE_UINT(tb_array_length(source.diagnostics), ==, 5);
    expect_diagnostic(&source, 0, TB_SHADER_ERROR, MAIN_PATH, 4, 16,
                      "no matching function for call to 'scale'");
    expect_diagnostic(&source, 1, TB_SHADER_WARNING, COMMON_PATH, 3, 12,
                      "implicit truncation of vector type");
    expect_diagnostic(&source, 2, TB_SHADER_ERROR, MAIN_PATH, 4, 5,
                      "Function uses features incompatible with the shader model");
    expect_diagnostic(&source, 3, TB_SHADER_ERROR, MAIN_PATH, 2, 17, "far along");
    expect_diagnostic(&source, 4, TB_SHADER_ERROR, MAIN_PATH, 2, 13, "between");
    tb_arena_destroy(source.arena);
}

TB_TEST(shader_dxc, what_names_no_place_in_the_text_keeps_its_own)
{
    rendered_source source = {0};
    render_source(&source, main_text, common_text);
    const size_t errors =
        read_messages(&source,
                      /* The summary before the validator's errors goes. */
                      "error: validation errors\n"
                      "fatal error: the compiler could not go on\n"
                      "error: invalid profile ps_6_99\n"
                      /* A line the text does not have is no place. */
                      MAIN_PATH ":99:1: error: past the end\n"
                      /* Another file stays as DXC names it. */
                      "other.hlsl:3:4: warning: elsewhere\n"
                      /* A place without numbers is all path. */
                      "Main.hlsl: error: odd place\n" MAIN_PATH ":x:1: error: not a line\n"
                      "Validation failed.\n"
                      /* A marker inside a message does not count. */
                      "the error: in the middle\n"
                      "warning: last, with no line break");
    TB_TEST_EXPECT_UINT(errors, ==, 5);
    TB_TEST_REQUIRE_UINT(tb_array_length(source.diagnostics), ==, 7);
    expect_diagnostic(&source, 0, TB_SHADER_ERROR, "", 0, 0, "the compiler could not go on");
    expect_diagnostic(&source, 1, TB_SHADER_ERROR, "", 0, 0, "invalid profile ps_6_99");
    expect_diagnostic(&source, 2, TB_SHADER_ERROR, "", 0, 0, "past the end");
    expect_diagnostic(&source, 3, TB_SHADER_WARNING, "other.hlsl", 3, 4, "elsewhere");
    expect_diagnostic(&source, 4, TB_SHADER_ERROR, "Main.hlsl", 0, 0, "odd place");
    expect_diagnostic(&source, 5, TB_SHADER_ERROR, MAIN_PATH ":x", 1, 0, "not a line");
    expect_diagnostic(&source, 6, TB_SHADER_WARNING, "", 0, 0, "last, with no line break");
    tb_arena_destroy(source.arena);
}

TB_TEST(shader_dxc, a_source_of_no_tokens_has_no_places)
{
    rendered_source source = {0};
    render_source(&source, "// nothing but a comment\n", "");
    TB_TEST_EXPECT_UINT(source.unit.token_count, ==, 0);
    TB_TEST_EXPECT_UINT(read_messages(&source, MAIN_PATH ":1:1: error: no entry point\n"), ==, 1);
    expect_diagnostic(&source, 0, TB_SHADER_ERROR, "", 0, 0, "no entry point");
    /* No messages, no diagnostics. */
    TB_TEST_EXPECT_UINT(read_messages(&source, ""), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_length(source.diagnostics), ==, 1);
    tb_arena_destroy(source.arena);
}

/* -------------------------------------------------------------------------
 * What a DXIL compile asks of DXC.
 * ------------------------------------------------------------------------- */

static bool write_with_fake(rendered_source *source, const tb_shader_compile_request *request,
                            tb_shader_dxil *dxil)
{
    const tb_shader_dxc_functions functions = {fake_compile, fake_release};
    fake.released = 0;
    const bool written = tb_shader_write_dxil(&source->unit, request, &functions, source->arena,
                                              source->diagnostics, dxil);
    TB_TEST_EXPECT_UINT(fake.released, ==, 1);
    return written;
}

TB_TEST(shader_dxc, dxc_compiles_the_rendered_text_for_the_stage_and_model)
{
    rendered_source source = {0};
    render_source(&source, main_text, common_text);
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXIL;
    request.dxc = (tb_dxc *)&fake;
    fake.result = TB_DXC_OK;
    fake.messages = "Engine/Private/Main.hlsl:7:9: warning: a warning\n";
    fake.error = "";
    tb_shader_dxil dxil = {0};
    TB_TEST_EXPECT_UINT(write_with_fake(&source, &request, &dxil), ==, true);
    /* DXC gets the request's DXC, the main path without the `/` it would
     * read as an option, the profile of the default shader model, and the
     * text without `#line`, whose places the offsets lead back from. */
    TB_TEST_EXPECT_POINTER(fake.dxc, ==, (tb_dxc *)&fake);
    TB_TEST_EXPECT_STRING(fake.name, ==, "Engine/Private/Main.hlsl");
    TB_TEST_EXPECT_STRING(fake.profile, ==, "ps_6_6");
    TB_TEST_EXPECT_STRING(fake.entry_point, ==, "main");
    TB_TEST_EXPECT_UINT(fake.flags, ==, 0);
    TB_TEST_EXPECT_STRING(fake.source, ==, source.rendered.text);
    /* The code is copied out of DXC's output, which is released. */
    TB_TEST_REQUIRE_UINT(dxil.code_size, ==, sizeof(fake_code));
    TB_TEST_EXPECT_POINTER(dxil.code, !=, (const void *)fake_code);
    TB_TEST_EXPECT_BYTES(dxil.code, fake_code, sizeof(fake_code));
    TB_TEST_EXPECT_UINT(((uintptr_t)dxil.code) % 4, ==, 0);
    TB_TEST_EXPECT_UINT(dxil.error_count, ==, 0);
    expect_diagnostic(&source, 0, TB_SHADER_WARNING, MAIN_PATH, 4, 16, "a warning");

    request.stage = TB_SHADER_STAGE_COMPUTE;
    request.shader_model = TB_SHADER_MODEL(6, 0);
    request.flags = TB_SHADER_COMPILE_DEBUG;
    fake.messages = "";
    TB_TEST_EXPECT_UINT(write_with_fake(&source, &request, &dxil), ==, true);
    TB_TEST_EXPECT_STRING(fake.profile, ==, "cs_6_0");
    TB_TEST_EXPECT_UINT(fake.flags, ==, TB_DXC_DEBUG);
    tb_arena_destroy(source.arena);
}

TB_TEST(shader_dxc, a_failed_compile_always_says_why)
{
    rendered_source source = {0};
    render_source(&source, main_text, common_text);
    tb_shader_compile_request request = {0};
    request.source.path = MAIN_PATH;
    request.entry_point = "main";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXIL;
    request.dxc = (tb_dxc *)&fake;
    fake.result = TB_DXC_FAILED;
    fake.messages = "Engine/Private/Main.hlsl:3:8: error: in the included file\n";
    fake.error = "Engine/Private/Main.hlsl:3:8: error: in the included file";
    tb_shader_dxil dxil = {0};
    TB_TEST_EXPECT_UINT(write_with_fake(&source, &request, &dxil), ==, false);
    TB_TEST_EXPECT_POINTER(dxil.code, ==, NULL);
    TB_TEST_EXPECT_UINT(dxil.error_count, ==, 1);
    expect_diagnostic(&source, 0, TB_SHADER_ERROR, COMMON_PATH, 3, 12, "in the included file");

    /* DXC failed, and its messages said nothing: its error says it. */
    fake.result = TB_DXC_NO_MEMORY;
    fake.messages = "";
    fake.error = "DXC failed to compile (HRESULT 0x8007000e)";
    TB_TEST_EXPECT_UINT(write_with_fake(&source, &request, &dxil), ==, false);
    TB_TEST_EXPECT_UINT(dxil.error_count, ==, 1);
    expect_diagnostic(&source, 1, TB_SHADER_ERROR, "", 0, 0,
                      "dxc: DXC failed to compile (HRESULT 0x8007000e)");
    tb_arena_destroy(source.arena);
}
