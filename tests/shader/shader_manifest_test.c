/* Manifests, the engine's own shader files, and finding a shader before
 * compiling it. A manifest is what the build compiles ahead of time, so a
 * line it misreads is a shader a game ships without; and a shader that is
 * found must be the one asked for, or none. These cases need no DXC: they
 * run on every platform. tests/shader/shader_library_d3d12_test.c compiles. */
#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "tb_test.h"

#include <string.h>

static tb_shader_manifest read_manifest(tb_arena *arena, const char *text, tb_shader_result expect)
{
    tb_shader_manifest manifest = {0};
    TB_TEST_EXPECT_INT(
        tb_shader_manifest_read(text, tb_str_length(text), "Test.manifest", arena, &manifest), ==,
        expect);
    return manifest;
}

TB_TEST(shader_manifest, each_line_is_a_shader)
{
    static const char text[] =
        "# The engine's shaders.\n"
        "\n"
        "/Engine/Private/Sky.hlsl  vertex MainVS\n"
        "\t/Engine/Private/Sky.hlsl\tpixel\tMainPS   # the sky's color\r\n"
        "/Engine/Private/Blur.hlsl compute MainCS RADIUS=4 HORIZONTAL EMPTY=\n"
        "/Game/Water.hlsl closesthit WaterHit";
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_manifest manifest = read_manifest(arena, text, TB_SHADER_OK);
    TB_TEST_REQUIRE_UINT(manifest.entry_count, ==, 4);
    TB_TEST_EXPECT_UINT(manifest.diagnostic_count, ==, 0);

    TB_TEST_EXPECT_STRING(manifest.entries[0].path, ==, "/Engine/Private/Sky.hlsl");
    TB_TEST_EXPECT_INT(manifest.entries[0].stage, ==, TB_SHADER_STAGE_VERTEX);
    TB_TEST_EXPECT_STRING(manifest.entries[0].entry_point, ==, "MainVS");
    TB_TEST_EXPECT_UINT(manifest.entries[0].define_count, ==, 0);
    TB_TEST_EXPECT_UINT(manifest.entries[0].line, ==, 3);

    TB_TEST_EXPECT_INT(manifest.entries[1].stage, ==, TB_SHADER_STAGE_PIXEL);
    TB_TEST_EXPECT_STRING(manifest.entries[1].entry_point, ==, "MainPS");
    TB_TEST_EXPECT_UINT(manifest.entries[1].line, ==, 4);

    const tb_shader_manifest_entry *const blur = &manifest.entries[2];
    TB_TEST_EXPECT_INT(blur->stage, ==, TB_SHADER_STAGE_COMPUTE);
    TB_TEST_REQUIRE_UINT(blur->define_count, ==, 3);
    TB_TEST_EXPECT_STRING(blur->defines[0].name, ==, "RADIUS");
    TB_TEST_EXPECT_STRING(blur->defines[0].value, ==, "4");
    TB_TEST_EXPECT_STRING(blur->defines[1].name, ==, "HORIZONTAL");
    TB_TEST_EXPECT_POINTER(blur->defines[1].value, ==, NULL);
    TB_TEST_EXPECT_STRING(blur->defines[2].name, ==, "EMPTY");
    TB_TEST_EXPECT_STRING(blur->defines[2].value, ==, "");

    TB_TEST_EXPECT_INT(manifest.entries[3].stage, ==, TB_SHADER_STAGE_CLOSEST_HIT);
    TB_TEST_EXPECT_UINT(manifest.entries[3].line, ==, 6);
    tb_arena_destroy(arena);
}

TB_TEST(shader_manifest, a_wrong_line_is_an_error_where_it_is)
{
    static const struct
    {
        const char *text;
        const char *diagnostic;
    } cases[] = {
        {"/Engine/Private/Sky.hlsl vertex\n",
         "Test.manifest(1,1): error: '/Engine/Private/Sky.hlsl' is not a shader: write its path, "
         "its stage and its entry point"},
        {"\nEngine/Sky.hlsl vertex MainVS\n",
         "Test.manifest(2,1): error: 'Engine/Sky.hlsl' is not a virtual shader path"},
        {"/Engine/Private/Sky.hlsl vertx MainVS\n",
         "Test.manifest(1,26): error: 'vertx' is no stage: write one as [shader(\"...\")] names "
         "it, such as pixel"},
        {"/Engine/Private/Sky.hlsl vertex 2Main\n",
         "Test.manifest(1,33): error: '2Main' is no function's name"},
        {"/Engine/Private/Sky.hlsl vertex MainVS =4\n",
         "Test.manifest(1,40): error: '=4' is no define: write NAME or NAME=VALUE"},
        {"/Engine/Private/Sky.hlsl vertex MainVS A-B\n",
         "Test.manifest(1,40): error: 'A-B' is no define: write NAME or NAME=VALUE"},
    };
    tb_arena *const arena = tb_arena_create(0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("%s", cases[index].text);
        const tb_shader_manifest manifest =
            read_manifest(arena, cases[index].text, TB_SHADER_FAILED);
        TB_TEST_EXPECT_UINT(manifest.entry_count, ==, 0);
        TB_TEST_REQUIRE_UINT(manifest.diagnostic_count, ==, 1);
        char line[256] = {0};
        (void)tb_shader_diagnostic_format(&manifest.diagnostics[0], line, sizeof(line));
        TB_TEST_EXPECT_STRING(line, ==, cases[index].diagnostic);
    }
    tb_arena_destroy(arena);
}

TB_TEST(shader_manifest, the_good_lines_of_a_wrong_manifest_are_read)
{
    static const char text[] = "/Engine/Private/Sky.hlsl vertex MainVS\n"
                               "/Engine/Private/Sky.hlsl\n"
                               "/Engine/Private/Sky.hlsl pixel MainPS\n";
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_manifest manifest = read_manifest(arena, text, TB_SHADER_FAILED);
    TB_TEST_EXPECT_UINT(manifest.entry_count, ==, 2);
    TB_TEST_EXPECT_UINT(manifest.diagnostic_count, ==, 1);
    tb_arena_destroy(arena);
}

TB_TEST(shader_manifest, a_line_has_a_bounded_number_of_defines_and_no_nul)
{
    char text[1024] = {0};
    size_t length = 0;
    memcpy(text, "/Engine/Private/Sky.hlsl vertex MainVS", 38);
    length = 38;
    for (size_t index = 0; index <= TB_SHADER_MANIFEST_MAX_DEFINES; index++)
    {
        memcpy(text + length, " D", 2);
        length += 2;
    }
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_manifest manifest = read_manifest(arena, text, TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(manifest.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(manifest.diagnostics[0].message, ==, "a shader has at most 32 defines");
    /* One fewer is fine. */
    text[length - 2] = '\0';
    manifest = read_manifest(arena, text, TB_SHADER_OK);
    TB_TEST_REQUIRE_UINT(manifest.entry_count, ==, 1);
    TB_TEST_EXPECT_UINT(manifest.entries[0].define_count, ==, TB_SHADER_MANIFEST_MAX_DEFINES);

    static const char with_nul[] = "/Engine/Private/Sky.hlsl\0 vertex MainVS\n";
    TB_TEST_EXPECT_INT(
        tb_shader_manifest_read(with_nul, sizeof(with_nul) - 1, "Test.manifest", arena, &manifest),
        ==, TB_SHADER_FAILED);
    TB_TEST_EXPECT_STRING(manifest.diagnostics[0].message, ==, "a manifest holds no NUL byte");
    TB_TEST_EXPECT_INT(tb_shader_manifest_read(NULL, 0, "Empty.manifest", arena, &manifest), ==,
                       TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(manifest.entry_count, ==, 0);
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * The engine's own files.
 * ------------------------------------------------------------------------- */

TB_TEST(shader_manifest, the_engine_carries_its_shader_files)
{
    /* Every shader the engine's manifest lists is one of its files, which
     * read and check as the build compiles them. */
    const tb_shader_loader loader = tb_shader_engine_loader();
    tb_arena *const arena = tb_arena_create(0);
    const char *text = NULL;
    size_t length = 0;
    TB_TEST_REQUIRE_INT(
        loader.load(loader.context, "/Engine/Private/Sky.hlsl", 24, arena, &text, &length), ==,
        TB_SHADER_LOAD_OK);
    TB_TEST_EXPECT_UINT(length, >, 0);
    TB_TEST_EXPECT_UINT(tb_str_find(text, 0, length, "MainPS"), !=, TB_STR_NOT_FOUND);
    TB_TEST_EXPECT_INT(
        loader.load(loader.context, "/Engine/Private/None.hlsl", 25, arena, &text, &length), ==,
        TB_SHADER_LOAD_NOT_FOUND);

    tb_shader_compile_request request = {0};
    tb_shader_compiled compiled = {0};
    request.source.path = "/Engine/Private/Sky.hlsl";
    request.source.loader = &loader;
    request.entry_point = "MainPS";
    request.stage = TB_SHADER_STAGE_PIXEL;
    TB_TEST_EXPECT_INT(tb_shader_compile(&request, arena, &compiled), ==, TB_SHADER_OK);
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * Finding a shader, or compiling it.
 * ------------------------------------------------------------------------- */

static const char main_text[] = "float4 MainPS() : SV_Target { return float4(1, 0, 0, 1); }\n";
static const unsigned char shipped_code[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4};

typedef struct lookup_case
{
    tb_shader_file file;
    tb_shader_compile_request request;
} lookup_case;

static void lookup_case_init(lookup_case *out)
{
    *out = (lookup_case){0};
    out->file = (tb_shader_file){"/Game/Main.hlsl", main_text, sizeof(main_text) - 1};
    out->request.source.path = "/Game/Main.hlsl";
    out->request.source.files = &out->file;
    out->request.source.file_count = 1;
    out->request.entry_point = "MainPS";
    out->request.stage = TB_SHADER_STAGE_PIXEL;
    out->request.target = TB_SHADER_TARGET_DXIL;
}

/* A library that holds the case's shader. */
static tb_shader_library *library_of(const lookup_case *source, tb_arena *arena)
{
    tb_shader_key key = {0};
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    TB_TEST_REQUIRE_INT(tb_shader_key_make(&source->request, &key), ==, TB_SHADER_OK);
    TB_TEST_REQUIRE(tb_shader_library_writer_add(writer, &key, shipped_code, sizeof(shipped_code)));
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, "a compiler", arena, &bytes, &size));
    TB_TEST_REQUIRE_INT(tb_shader_library_open(bytes, size, &library), ==, TB_SHADER_LIBRARY_OK);
    tb_shader_library_writer_destroy(writer);
    return library;
}

TB_TEST(shader_manifest, a_library_gives_its_shader_without_a_compiler)
{
    lookup_case source = {0};
    tb_arena *const arena = tb_arena_create(0);
    lookup_case_init(&source);
    tb_shader_library *const library = library_of(&source, arena);
    tb_shader_lookup lookup = {.library = library, .library_only = true};
    tb_shader_compiled compiled = {0};
    tb_shader_key key = {0};
    tb_shader_origin origin = TB_SHADER_ORIGIN_COMPILE;
    TB_TEST_REQUIRE_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_OK);
    TB_TEST_EXPECT_INT(origin, ==, TB_SHADER_ORIGIN_LIBRARY);
    TB_TEST_REQUIRE_UINT(compiled.code_size, ==, sizeof(shipped_code));
    TB_TEST_EXPECT_BYTES(compiled.code, shipped_code, sizeof(shipped_code));

    /* Another entry point is another shader, which the library lacks. */
    source.request.entry_point = "OtherPS";
    TB_TEST_EXPECT_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_UNAVAILABLE);
    TB_TEST_REQUIRE_UINT(compiled.diagnostic_count, ==, 1);
    TB_TEST_EXPECT_STRING(compiled.diagnostics[0].message, ==,
                          "the shader is in no library, and compiling at run time is off");
    TB_TEST_EXPECT_POINTER(compiled.code, ==, NULL);

    /* With compiling allowed, but no compiler, it is still unavailable. */
    lookup.library_only = false;
    TB_TEST_EXPECT_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_UNAVAILABLE);
    TB_TEST_EXPECT_STRING(compiled.diagnostics[0].message, ==,
                          "the shader is in no library, and there is no compiler for its target");
    tb_shader_library_close(library);
    tb_arena_destroy(arena);
}

TB_TEST(shader_manifest, reflection_and_wrong_sources_are_compiled)
{
    static const char broken[] = "float4 MainPS() : SV_Target { return nothing; }\n";
    static const char unreadable[] = "#error no\n";
    lookup_case source = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_lookup lookup = {.library_only = true};
    tb_shader_compiled compiled = {0};
    tb_shader_key key = {0};
    tb_shader_origin origin = TB_SHADER_ORIGIN_LIBRARY;

    /* The reflection alone needs no compiler, and comes with every compile. */
    lookup_case_init(&source);
    source.request.target = TB_SHADER_TARGET_NONE;
    TB_TEST_EXPECT_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_OK);
    TB_TEST_EXPECT_INT(origin, ==, TB_SHADER_ORIGIN_COMPILE);
    TB_TEST_EXPECT_UINT(compiled.reflection.output_count, ==, 1);

    /* A source that checks wrongly has a key, and is in no library. */
    lookup_case_init(&source);
    source.file.text = broken;
    source.file.length = sizeof(broken) - 1;
    TB_TEST_EXPECT_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_UNAVAILABLE);

    /* One that cannot be read has no key: the compile says why. */
    source.file.text = unreadable;
    source.file.length = sizeof(unreadable) - 1;
    TB_TEST_EXPECT_INT(
        tb_shader_compile_cached(&source.request, &lookup, arena, &compiled, &key, &origin), ==,
        TB_SHADER_FAILED);
    TB_TEST_REQUIRE_UINT(compiled.diagnostic_count, >, 0);
    TB_TEST_EXPECT_STRING(compiled.diagnostics[0].message, ==, "#error no");
    const tb_shader_key zero = {0};
    TB_TEST_EXPECT_BYTES(key.bytes, zero.bytes, TB_SHADER_KEY_SIZE);
    TB_TEST_EXPECT_STRING(tb_shader_result_name(TB_SHADER_UNAVAILABLE), ==, "unavailable");
    tb_arena_destroy(arena);
}
