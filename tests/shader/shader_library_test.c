/* Shader libraries: what a writer writes, a library opens and finds,
 * byte for byte; and a library that is damaged anywhere, or of another
 * version, is refused whole, never read in part, since its code goes to a
 * GPU driver as it is. */
#include "badger/arena.h"
#include "badger/fs.h"
#include "badger/path.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

#define COMPILER "dxcompiler 1.8 (4775, 1b4f3c2de) dxil 1.8 (4775, 1b4f3c2de)"

/* Code of sizes that are not multiples of 8, so the library pads between them. */
static const unsigned char code_a[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4, 5, 6, 7};
static const unsigned char code_b[] = {'D', 'X', 'B', 'C', 9};
static const unsigned char code_c[] = {'D', 'X', 'B', 'C', 1, 1, 2, 3, 5, 8, 13, 21, 34};

static tb_shader_key key_numbered(unsigned number)
{
    tb_shader_key shader = {0};
    tb_shader_key key = {0};
    shader.bytes[0] = (uint8_t)number;
    /* A digest, so the keys' order is not the order they are numbered in. */
    tb_shader_cache_key(&shader, "a compiler", &key);
    return key;
}

/* Write a library of the three codes, added in the order given. */
static void write_three(const unsigned order[3], tb_arena *arena, const void **bytes, size_t *size)
{
    const unsigned char *const codes[3] = {code_a, code_b, code_c};
    const size_t sizes[3] = {sizeof(code_a), sizeof(code_b), sizeof(code_c)};
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    for (size_t index = 0; index < 3; index++)
    {
        const unsigned which = order[index];
        const tb_shader_key key = key_numbered(which);
        TB_TEST_REQUIRE(tb_shader_library_writer_add(writer, &key, codes[which], sizes[which]));
    }
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, COMPILER, arena, bytes, size));
    tb_shader_library_writer_destroy(writer);
}

TB_TEST(shader_library, a_written_library_opens_and_finds_every_shader)
{
    static const unsigned order[3] = {2, 0, 1};
    const unsigned char *const codes[3] = {code_a, code_b, code_c};
    const size_t sizes[3] = {sizeof(code_a), sizeof(code_b), sizeof(code_c)};
    tb_arena *const arena = tb_arena_create(0);
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    write_three(order, arena, &bytes, &size);
    TB_TEST_REQUIRE_INT(tb_shader_library_open(bytes, size, &library), ==, TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, 3);
    TB_TEST_EXPECT_STRING(tb_shader_library_compiler(library), ==, COMPILER);
    for (unsigned which = 0; which < 3; which++)
    {
        const tb_shader_key key = key_numbered(which);
        const void *code = NULL;
        size_t code_size = 0;
        TB_TEST_CONTEXT("shader %u", which);
        TB_TEST_REQUIRE(tb_shader_library_find(library, &key, &code, &code_size));
        TB_TEST_REQUIRE_UINT(code_size, ==, sizes[which]);
        TB_TEST_EXPECT_BYTES(code, codes[which], sizes[which]);
        TB_TEST_EXPECT_UINT((uintptr_t)code % 8u, ==, 0);
    }
    const tb_shader_key missing = key_numbered(3);
    const void *code = bytes;
    size_t code_size = 1;
    TB_TEST_EXPECT(!tb_shader_library_find(library, &missing, &code, &code_size));
    TB_TEST_EXPECT_POINTER(code, ==, NULL);
    TB_TEST_EXPECT_UINT(code_size, ==, 0);
    tb_shader_library_close(library);
    tb_shader_library_close(NULL);
    tb_arena_destroy(arena);
}

TB_TEST(shader_library, the_same_shaders_write_the_same_bytes_in_any_order)
{
    /* A build that compiles in parallel adds shaders in the order they
     * finish; its library must not change with that order. */
    static const unsigned first[3] = {0, 1, 2};
    static const unsigned second[3] = {2, 1, 0};
    tb_arena *const arena = tb_arena_create(0);
    const void *a = NULL;
    const void *b = NULL;
    size_t a_size = 0;
    size_t b_size = 0;
    write_three(first, arena, &a, &a_size);
    write_three(second, arena, &b, &b_size);
    TB_TEST_REQUIRE_UINT(a_size, ==, b_size);
    TB_TEST_EXPECT_BYTES(a, b, a_size);
    tb_arena_destroy(arena);
}

TB_TEST(shader_library, a_key_is_added_once)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    const tb_shader_key key = key_numbered(0);
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    TB_TEST_EXPECT(tb_shader_library_writer_add(writer, &key, code_a, sizeof(code_a)));
    /* Two entries of a manifest may compile alike. */
    TB_TEST_EXPECT(tb_shader_library_writer_add(writer, &key, code_a, sizeof(code_a)));
    /* One key with two codes is a mistake the library cannot hold. */
    TB_TEST_EXPECT(!tb_shader_library_writer_add(writer, &key, code_b, sizeof(code_b)));
    TB_TEST_EXPECT(!tb_shader_library_writer_add(writer, &key, code_a, sizeof(code_a) - 1));
    TB_TEST_EXPECT(!tb_shader_library_writer_add(writer, &key, code_a, 0));
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, COMPILER, arena, &bytes, &size));
    TB_TEST_REQUIRE_INT(tb_shader_library_open(bytes, size, &library), ==, TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, 1);
    tb_shader_library_close(library);
    tb_shader_library_writer_destroy(writer);
    tb_shader_library_writer_destroy(NULL);
    tb_arena_destroy(arena);
}

TB_TEST(shader_library, an_empty_library_and_a_long_compiler_name)
{
    tb_arena *const arena = tb_arena_create(0);
    tb_shader_library_writer *const writer = tb_shader_library_writer_create();
    char compiler[TB_SHADER_LIBRARY_MAX_COMPILER + 2] = {0};
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    memset(compiler, 'x', TB_SHADER_LIBRARY_MAX_COMPILER);
    TB_TEST_REQUIRE(tb_shader_library_writer_finish(writer, compiler, arena, &bytes, &size));
    TB_TEST_REQUIRE_INT(tb_shader_library_open(bytes, size, &library), ==, TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, 0);
    TB_TEST_EXPECT_STRING(tb_shader_library_compiler(library), ==, compiler);
    tb_shader_library_close(library);
    compiler[TB_SHADER_LIBRARY_MAX_COMPILER] = 'x';
    TB_TEST_EXPECT(!tb_shader_library_writer_finish(writer, compiler, arena, &bytes, &size));
    tb_shader_library_writer_destroy(writer);
    tb_arena_destroy(arena);
}

static tb_shader_library_result open_result(const void *bytes, size_t size)
{
    tb_shader_library *library = NULL;
    const tb_shader_library_result result = tb_shader_library_open(bytes, size, &library);
    if (result != TB_SHADER_LIBRARY_OK)
    {
        TB_TEST_EXPECT_POINTER(library, ==, NULL);
    }
    tb_shader_library_close(library);
    return result;
}

TB_TEST(shader_library, a_damaged_library_is_refused_whole)
{
    static const unsigned order[3] = {0, 1, 2};
    tb_arena *const arena = tb_arena_create(0);
    const void *bytes = NULL;
    size_t size = 0;
    write_three(order, arena, &bytes, &size);
    unsigned char *const copy = tb_arena_alloc(arena, size + 8, 8);
    memcpy(copy, bytes, size);

    /* Every truncation, from nothing to all but the last byte. */
    for (size_t length = 0; length < size; length++)
    {
        TB_TEST_CONTEXT("cut to %zu bytes", length);
        TB_TEST_EXPECT_INT(open_result(copy, length), ==, TB_SHADER_LIBRARY_DAMAGED);
    }
    /* A byte more. */
    copy[size] = 0;
    TB_TEST_EXPECT_INT(open_result(copy, size + 1), ==, TB_SHADER_LIBRARY_DAMAGED);
    /* Every byte changed: the header, the compiler's name, the table, the
     * code and the padding between. The version's bytes make another
     * version, which is refused as one. */
    for (size_t at = 0; at < size; at++)
    {
        TB_TEST_CONTEXT("byte %zu changed", at);
        copy[at] ^= 0x5Au;
        const tb_shader_library_result result = open_result(copy, size);
        copy[at] ^= 0x5Au;
        TB_TEST_EXPECT_INT(
            result, ==, at >= 4 && at < 8 ? TB_SHADER_LIBRARY_VERSION : TB_SHADER_LIBRARY_DAMAGED);
    }
    /* Untouched, it opens. */
    TB_TEST_EXPECT_INT(open_result(copy, size), ==, TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_INT(open_result(NULL, 0), ==, TB_SHADER_LIBRARY_DAMAGED);
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * Through the VFS.
 * ------------------------------------------------------------------------- */

#define PATH_CAPACITY ((size_t)32768)

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

static void join(const char *first, const char *second, char *out)
{
    size_t required = 0;
    TB_TEST_REQUIRE_INT(tb_path_join(tb_fs_native_style(), view(first), view(second), out,
                                     PATH_CAPACITY, &required),
                        ==, TB_PATH_OK);
}

#define NAME "Shaders/D3D12.tbsl"
#define DAMAGED_NAME "Shaders/Damaged.tbsl"

TB_TEST(shader_library, a_library_loads_through_the_vfs)
{
    static const unsigned order[3] = {1, 2, 0};
    tb_arena *const arena = tb_arena_create(0);
    char root[PATH_CAPACITY] = {0};
    char directory[PATH_CAPACITY] = {0};
    char path[PATH_CAPACITY] = {0};
    char damaged_path[PATH_CAPACITY] = {0};
    const void *bytes = NULL;
    size_t size = 0;
    tb_shader_library *library = NULL;
    write_three(order, arena, &bytes, &size);
    TB_TEST_REQUIRE_INT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    join(root, "Shaders", directory);
    join(directory, "D3D12.tbsl", path);
    join(directory, "Damaged.tbsl", damaged_path);
    TB_TEST_REQUIRE_INT(tb_fs_make_directory(view(directory)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_fs_write_file(view(path), bytes, size), ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(tb_fs_write_file(view(damaged_path), bytes, size - 1), ==, TB_FS_OK);
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_REQUIRE_INT(tb_vfs_mount_directory(vfs, "Engine", 6, root, 0), ==, TB_VFS_OK);

    TB_TEST_REQUIRE_INT(
        tb_shader_library_load(vfs, "Engine/" NAME, sizeof("Engine/" NAME) - 1, &library), ==,
        TB_SHADER_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_shader_library_count(library), ==, 3);
    const tb_shader_key key = key_numbered(2);
    const void *code = NULL;
    size_t code_size = 0;
    TB_TEST_REQUIRE(tb_shader_library_find(library, &key, &code, &code_size));
    TB_TEST_EXPECT_BYTES(code, code_c, sizeof(code_c));
    tb_shader_library_close(library);

    TB_TEST_EXPECT_INT(tb_shader_library_load(vfs, "Engine/Shaders/None.tbsl",
                                              sizeof("Engine/Shaders/None.tbsl") - 1, &library),
                       ==, TB_SHADER_LIBRARY_NOT_FOUND);
    TB_TEST_EXPECT_INT(
        tb_shader_library_load(vfs, "Engine/Shaders", sizeof("Engine/Shaders") - 1, &library), ==,
        TB_SHADER_LIBRARY_NOT_FOUND);
    TB_TEST_EXPECT_INT(tb_shader_library_load(vfs, "Engine/" DAMAGED_NAME,
                                              sizeof("Engine/" DAMAGED_NAME) - 1, &library),
                       ==, TB_SHADER_LIBRARY_DAMAGED);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
    TB_TEST_EXPECT_INT(tb_shader_library_load(vfs, "../x", 4, &library), ==,
                       TB_SHADER_LIBRARY_READ_FAILED);

    tb_vfs_destroy(vfs);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(damaged_path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(directory)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(root)), ==, TB_FS_OK);
    tb_arena_destroy(arena);
}

TB_TEST(shader_library, results_have_names)
{
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name(TB_SHADER_LIBRARY_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name(TB_SHADER_LIBRARY_NOT_FOUND), ==,
                          "not found");
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name(TB_SHADER_LIBRARY_READ_FAILED), ==,
                          "read failed");
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name(TB_SHADER_LIBRARY_DAMAGED), ==, "damaged");
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name(TB_SHADER_LIBRARY_VERSION), ==,
                          "another version");
    TB_TEST_EXPECT_STRING(tb_shader_library_result_name((tb_shader_library_result)99), ==,
                          "unknown result");
}
