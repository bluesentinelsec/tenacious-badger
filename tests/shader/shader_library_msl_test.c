/* Every engine shader compiles on each build host. On macOS the build also
 * cooks the manifest, so the key and every byte can be compared at runtime. */
#include "badger/arena.h"
#include "badger/fs.h"
#include "badger/shader.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "tb_test.h"

TB_TEST(shader_msl, engine_manifest_compiles_for_metal)
{
    tb_fs_buffer manifest_text = {0};
    tb_shader_manifest manifest = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_path_view path = {TB_TEST_SHADER_MANIFEST, sizeof(TB_TEST_SHADER_MANIFEST) - 1};
    TB_TEST_REQUIRE_INT(tb_fs_read_file(path, NULL, 1u << 20, &manifest_text), ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_shader_manifest_read((const char *)manifest_text.data,
                                                manifest_text.length, "Shaders.manifest", arena,
                                                &manifest),
                        ==, TB_SHADER_OK);
    const tb_shader_loader loader = tb_shader_engine_loader();
    tb_shader_library *library = NULL;
    tb_fs_buffer bytes = {0};
#ifdef TB_TEST_BUILT_METAL_LIBRARY
    const char name[] = TB_TEST_SHADER_DIRECTORY "/Shaders/metal.tbsl";
    TB_TEST_REQUIRE_INT(tb_fs_read_file((tb_path_view){name, sizeof(name) - 1}, NULL,
                                        TB_SHADER_LIBRARY_MAX_SIZE, &bytes),
                        ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_shader_library_open(bytes.data, bytes.length, &library), ==,
                        TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, manifest.entry_count);
#endif
    for (size_t i = 0; i < manifest.entry_count; i++)
    {
        const tb_shader_manifest_entry *const entry = &manifest.entries[i];
        TB_TEST_CONTEXT("%s %s", entry->path, entry->entry_point);
        tb_shader_compile_request request = {.source = {.path = entry->path,
                                                        .loader = &loader,
                                                        .defines = entry->defines,
                                                        .define_count = entry->define_count},
                                             .stage = entry->stage,
                                             .entry_point = entry->entry_point,
                                             .target = TB_SHADER_TARGET_MSL};
        tb_shader_compiled compiled = {0};
        TB_TEST_REQUIRE_UINT(tb_shader_compile(&request, arena, &compiled), ==, TB_SHADER_OK);
        TB_TEST_REQUIRE_UINT(compiled.code_size, >, 0);
        if (library != NULL)
        {
            tb_shader_key key = {0};
            const void *ahead = NULL;
            size_t size = 0;
            TB_TEST_REQUIRE_UINT(tb_shader_key_make(&request, &key), ==, TB_SHADER_OK);
            TB_TEST_REQUIRE(tb_shader_library_find(library, &key, &ahead, &size));
            TB_TEST_REQUIRE_UINT(size, ==, compiled.code_size);
            TB_TEST_EXPECT_BYTES(compiled.code, ahead, size);
        }
    }
    tb_shader_library_close(library);
    tb_fs_buffer_free(&bytes);
    tb_fs_buffer_free(&manifest_text);
    tb_arena_destroy(arena);
}
