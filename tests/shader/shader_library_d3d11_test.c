/* The engine's shaders for D3D11, compiled ahead of time by badger-shaderc
 * when the engine is built, and compiled again here at run time: each must
 * give the same bytes, and be found in the library by the key the engine
 * computes. Skips where D3DCompiler is unavailable, and where the build
 * made no library (TB_BUILD_SHADERS off). */
#include "badger/arena.h"
#include "badger/d3dcompiler.h"
#include "badger/fs.h"
#include "badger/path.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "tb_test.h"

#include <string.h>

#define LIBRARY_PATH TB_TEST_SHADER_DIRECTORY "/Shaders/d3d11.tbsl"

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

TB_TEST(shader_library_d3d11, ahead_of_time_and_run_time_compiles_give_the_same_bytes)
{
    tb_d3dcompiler *compiler = NULL;
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    tb_fs_buffer library_bytes = {0};
    tb_fs_buffer manifest_text = {0};
    tb_shader_library *library = NULL;
    tb_shader_manifest manifest = {0};
    if (tb_d3dcompiler_open(&compiler, &error) != TB_D3DCOMPILER_OK)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    if (tb_fs_read_file(view(LIBRARY_PATH), NULL, TB_SHADER_LIBRARY_MAX_SIZE, &library_bytes) !=
        TB_FS_OK)
    {
        tb_d3dcompiler_close(compiler);
        TB_TEST_SKIP("the build made no shader library at %s", LIBRARY_PATH);
    }
    tb_arena *const arena = tb_arena_create(0);
    TB_TEST_REQUIRE_INT(tb_shader_library_open(library_bytes.data, library_bytes.length, &library),
                        ==, TB_SHADER_LIBRARY_OK);
    /* The library records the compiler that wrote it: this machine's. */
    TB_TEST_EXPECT_STRING(tb_shader_library_compiler(library), ==,
                          tb_d3dcompiler_version(compiler));
    TB_TEST_REQUIRE_INT(
        tb_fs_read_file(view(TB_TEST_SHADER_MANIFEST), NULL, 1u << 20, &manifest_text), ==,
        TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_shader_manifest_read((const char *)manifest_text.data,
                                                manifest_text.length, "Shaders.manifest", arena,
                                                &manifest),
                        ==, TB_SHADER_OK);
    TB_TEST_EXPECT_UINT(manifest.entry_count, >, 0);
    /* Every shader of the engine's runs on D3D11. */
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, manifest.entry_count);
    const tb_shader_loader loader = tb_shader_engine_loader();
    for (size_t index = 0; index < manifest.entry_count; index++)
    {
        const tb_shader_manifest_entry *const entry = &manifest.entries[index];
        TB_TEST_CONTEXT("%s %s", entry->path, entry->entry_point);
        tb_shader_compile_request request = {0};
        request.source.path = entry->path;
        request.source.defines = entry->defines;
        request.source.define_count = entry->define_count;
        request.source.loader = &loader;
        request.entry_point = entry->entry_point;
        request.stage = entry->stage;
        request.target = TB_SHADER_TARGET_DXBC;
        request.d3dcompiler = compiler;
        tb_shader_key key = {0};
        const void *ahead = NULL;
        size_t ahead_size = 0;
        tb_shader_compiled compiled = {0};
        TB_TEST_REQUIRE_INT(tb_shader_key_make(&request, &key), ==, TB_SHADER_OK);
        TB_TEST_REQUIRE(tb_shader_library_find(library, &key, &ahead, &ahead_size));
        TB_TEST_REQUIRE_INT(tb_shader_compile(&request, arena, &compiled), ==, TB_SHADER_OK);
        TB_TEST_REQUIRE_UINT(compiled.code_size, ==, ahead_size);
        TB_TEST_EXPECT_BYTES(compiled.code, ahead, ahead_size);
    }
    tb_shader_library_close(library);
    tb_fs_buffer_free(&manifest_text);
    tb_fs_buffer_free(&library_bytes);
    tb_arena_destroy(arena);
    tb_d3dcompiler_close(compiler);
}

TB_TEST(shader_library_d3d11, the_cache_keeps_what_was_compiled_by_its_compiler)
{
    /* The cache key names D3DCompiler's version, so that another compiler
     * compiles again; a key of DXC's for the same shader is another. */
    tb_d3dcompiler *compiler = NULL;
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    char root[32768] = {0};
    tb_shader_cache *cache = NULL;
    if (tb_d3dcompiler_open(&compiler, &error) != TB_D3DCOMPILER_OK)
    {
        TB_TEST_SKIP("%s", error.message);
    }
    tb_arena *const arena = tb_arena_create(0);
    TB_TEST_REQUIRE_INT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_shader_cache_open(root, &cache), ==, TB_SHADER_CACHE_OK);
    const tb_shader_loader loader = tb_shader_engine_loader();
    tb_shader_compile_request request = {0};
    request.source.path = "/Engine/Private/Mesh.hlsl";
    request.source.loader = &loader;
    request.entry_point = "MainPS";
    request.stage = TB_SHADER_STAGE_PIXEL;
    request.target = TB_SHADER_TARGET_DXBC;
    request.d3dcompiler = compiler;
    const tb_shader_lookup lookup = {.cache = cache};
    tb_shader_compiled first = {0};
    tb_shader_compiled second = {0};
    tb_shader_key first_key = {0};
    tb_shader_key second_key = {0};
    tb_shader_origin origin = TB_SHADER_ORIGIN_LIBRARY;
    TB_TEST_REQUIRE_INT(
        tb_shader_compile_cached(&request, &lookup, arena, &first, &first_key, &origin), ==,
        TB_SHADER_OK);
    TB_TEST_EXPECT_INT(origin, ==, TB_SHADER_ORIGIN_COMPILE);
    TB_TEST_REQUIRE_INT(
        tb_shader_compile_cached(&request, &lookup, arena, &second, &second_key, &origin), ==,
        TB_SHADER_OK);
    TB_TEST_EXPECT_INT(origin, ==, TB_SHADER_ORIGIN_CACHE);
    TB_TEST_REQUIRE_UINT(second.code_size, ==, first.code_size);
    TB_TEST_EXPECT_BYTES(second.code, first.code, first.code_size);

    /* The same source for D3D12 has a key of its own. */
    tb_shader_compile_request dxil = request;
    dxil.target = TB_SHADER_TARGET_DXIL;
    tb_shader_key dxil_key = {0};
    TB_TEST_REQUIRE_INT(tb_shader_key_make(&dxil, &dxil_key), ==, TB_SHADER_OK);
    TB_TEST_EXPECT(memcmp(dxil_key.bytes, first_key.bytes, TB_SHADER_KEY_SIZE) != 0);

    /* Remove the one entry, then the cache. */
    tb_shader_key entry = {0};
    char name[TB_SHADER_KEY_TEXT_CAPACITY + 8] = {0};
    char path[32768] = {0};
    size_t required = 0;
    tb_shader_cache_key(&first_key, tb_d3dcompiler_version(compiler), &entry);
    tb_shader_key_format(&entry, name);
    memcpy(name + TB_SHADER_KEY_SIZE * 2, ".tbsc", sizeof(".tbsc"));
    TB_TEST_REQUIRE_INT(
        tb_path_join(tb_fs_native_style(), view(root), view(name), path, sizeof(path), &required),
        ==, TB_PATH_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(root)), ==, TB_FS_OK);
    tb_shader_cache_close(cache);
    tb_arena_destroy(arena);
    tb_d3dcompiler_close(compiler);
}
