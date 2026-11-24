/* Shader keys and the shader cache. A key must change with everything that
 * changes what a compile writes, or a stale shader would be found for a
 * changed source; and the cache must treat every entry it cannot trust as a
 * miss, never as code, since the code goes to a GPU driver as it is. */
#include "badger/arena.h"
#include "badger/fs.h"
#include "badger/path.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

#define MAIN_PATH "/Engine/Private/Main.hlsl"
#define COMMON_PATH "/Engine/Public/Common.hlsli"

static const char main_text[] = "#include \"/Engine/Public/Common.hlsli\"\n"
                                "float4 MainPS() : SV_Target { return Tint * SCALE; }\n"
                                "float4 OtherPS() : SV_Target { return Tint; }\n";
static const char common_text[] = "static const float4 Tint = float4(1, 0, 0, 1);\n";

/* The sources and the request every case varies one input of. */
typedef struct key_case
{
    tb_shader_file files[2];
    tb_shader_define defines[2];
    tb_shader_compile_request request;
} key_case;

static void key_case_init(key_case *out)
{
    const key_case empty = {0};
    *out = empty;
    out->files[0] = (tb_shader_file){MAIN_PATH, main_text, sizeof(main_text) - 1};
    out->files[1] = (tb_shader_file){COMMON_PATH, common_text, sizeof(common_text) - 1};
    out->defines[0] = (tb_shader_define){"SCALE", "2"};
    out->defines[1] = (tb_shader_define){"QUALITY", NULL};
    out->request.source.path = MAIN_PATH;
    out->request.source.files = out->files;
    out->request.source.file_count = 2;
    out->request.source.defines = out->defines;
    out->request.source.define_count = 1;
    out->request.entry_point = "MainPS";
    out->request.stage = TB_SHADER_STAGE_PIXEL;
    out->request.target = TB_SHADER_TARGET_DXIL;
}

static tb_shader_key key_of(const key_case *source)
{
    tb_shader_key key = {0};
    TB_TEST_REQUIRE_INT(tb_shader_key_make(&source->request, &key), ==, TB_SHADER_OK);
    return key;
}

static bool keys_equal(const tb_shader_key *left, const tb_shader_key *right)
{
    return memcmp(left->bytes, right->bytes, TB_SHADER_KEY_SIZE) == 0;
}

TB_TEST(shader_cache, the_same_request_has_the_same_key)
{
    key_case first = {0};
    key_case second = {0};
    key_case_init(&first);
    key_case_init(&second);
    const tb_shader_key a = key_of(&first);
    const tb_shader_key b = key_of(&second);
    TB_TEST_EXPECT(keys_equal(&a, &b));
    /* DXC is not part of the shader key: a library is found without one. */
    second.request.dxc = (struct tb_dxc *)&second;
    const tb_shader_key c = key_of(&second);
    TB_TEST_EXPECT(keys_equal(&a, &c));
}

TB_TEST(shader_cache, every_input_changes_the_key)
{
    static const char other_main[] = "#include \"/Engine/Public/Common.hlsli\"\n"
                                     "float4 MainPS() : SV_Target { return Tint * SCALE * 2; }\n"
                                     "float4 OtherPS() : SV_Target { return Tint; }\n";
    static const char other_common[] = "static const float4 Tint = float4(0, 1, 0, 1);\n";
    /* The same code a line lower: where code sits shows in DXC's debug
     * information and its messages. */
    static const char moved_common[] = "\nstatic const float4 Tint = float4(1, 0, 0, 1);\n";
    static const char *const names[] = {"the main file",     "an included file", "where code sits",
                                        "a define's value",  "another define",   "the entry point",
                                        "the stage",         "the target",       "the shader model",
                                        "the compile flags", "the main path"};
    tb_shader_key keys[TB_TEST_COUNT_OF(names) + 1] = {{{0}}};
    key_case source = {0};
    key_case_init(&source);
    keys[0] = key_of(&source);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        TB_TEST_CONTEXT("changing %s", names[index]);
        key_case_init(&source);
        switch (index)
        {
        case 0:
            source.files[0].text = other_main;
            source.files[0].length = sizeof(other_main) - 1;
            break;
        case 1:
            source.files[1].text = other_common;
            source.files[1].length = sizeof(other_common) - 1;
            break;
        case 2:
            source.files[1].text = moved_common;
            source.files[1].length = sizeof(moved_common) - 1;
            break;
        case 3:
            source.defines[0].value = "3";
            break;
        case 4:
            source.request.source.define_count = 2;
            break;
        case 5:
            source.request.entry_point = "OtherPS";
            break;
        case 6:
            source.request.stage = TB_SHADER_STAGE_VERTEX;
            break;
        case 7:
            source.request.target = TB_SHADER_TARGET_NONE;
            break;
        case 8:
            source.request.shader_model = TB_SHADER_MODEL(6, 7);
            break;
        case 9:
            source.request.flags = TB_SHADER_COMPILE_DEBUG;
            break;
        default:
            source.files[0].path = "/Engine/Private/Other.hlsl";
            source.request.source.path = "/Engine/Private/Other.hlsl";
            break;
        }
        keys[index + 1] = key_of(&source);
        for (size_t other = 0; other <= index; other++)
        {
            TB_TEST_EXPECT(!keys_equal(&keys[index + 1], &keys[other]));
        }
    }
}

TB_TEST(shader_cache, the_default_shader_model_and_a_define_of_one_are_spelled_out)
{
    /* A request that leaves the shader model at zero asks for the default,
     * and a define without a value is `1`: each compiles as its spelled-out
     * form does, so each has its key. */
    key_case source = {0};
    key_case_init(&source);
    const tb_shader_key implicit = key_of(&source);
    source.request.shader_model = TB_SHADER_MODEL_DEFAULT;
    const tb_shader_key spelled = key_of(&source);
    TB_TEST_EXPECT(keys_equal(&implicit, &spelled));

    source.request.source.define_count = 2;
    const tb_shader_key without_value = key_of(&source);
    source.defines[1].value = "1";
    const tb_shader_key with_one = key_of(&source);
    TB_TEST_EXPECT(keys_equal(&without_value, &with_one));
}

TB_TEST(shader_cache, a_request_that_cannot_be_read_has_no_key)
{
    static const char broken[] = "#error not this one\n";
    key_case source = {0};
    tb_shader_key key = {0};
    key_case_init(&source);
    source.request.entry_point = "";
    TB_TEST_EXPECT_INT(tb_shader_key_make(&source.request, &key), ==, TB_SHADER_INVALID);
    key_case_init(&source);
    source.request.stage = (tb_shader_stage)99;
    TB_TEST_EXPECT_INT(tb_shader_key_make(&source.request, &key), ==, TB_SHADER_INVALID);
    key_case_init(&source);
    source.request.source.path = "not a path";
    TB_TEST_EXPECT_INT(tb_shader_key_make(&source.request, &key), ==, TB_SHADER_INVALID);
    key_case_init(&source);
    source.files[0].text = broken;
    source.files[0].length = sizeof(broken) - 1;
    TB_TEST_EXPECT_INT(tb_shader_key_make(&source.request, &key), ==, TB_SHADER_FAILED);
    const tb_shader_key zero = {0};
    TB_TEST_EXPECT(keys_equal(&key, &zero));
}

TB_TEST(shader_cache, the_cache_key_names_the_compiler)
{
    key_case source = {0};
    key_case_init(&source);
    const tb_shader_key shader = key_of(&source);
    source.request.entry_point = "OtherPS";
    const tb_shader_key other_shader = key_of(&source);
    tb_shader_key a = {0};
    tb_shader_key b = {0};
    tb_shader_key c = {0};
    tb_shader_key d = {0};
    tb_shader_cache_key(&shader, "dxcompiler 1.8 (4775, 1b4f3c2de)", &a);
    tb_shader_cache_key(&shader, "dxcompiler 1.8 (4775, 1b4f3c2de)", &b);
    tb_shader_cache_key(&shader, "dxcompiler 1.8 (4776, 0a3e5d1c7)", &c);
    tb_shader_cache_key(&other_shader, "dxcompiler 1.8 (4775, 1b4f3c2de)", &d);
    TB_TEST_EXPECT(keys_equal(&a, &b));
    TB_TEST_EXPECT(!keys_equal(&a, &c));
    TB_TEST_EXPECT(!keys_equal(&a, &d));
    /* A cache key is never its shader key. */
    TB_TEST_EXPECT(!keys_equal(&a, &shader));
}

TB_TEST(shader_cache, a_key_is_written_in_hexadecimal)
{
    tb_shader_key key = {0};
    char text[TB_SHADER_KEY_TEXT_CAPACITY] = {0};
    for (size_t index = 0; index < TB_SHADER_KEY_SIZE; index++)
    {
        key.bytes[index] = (uint8_t)(index * 9u);
    }
    tb_shader_key_format(&key, text);
    TB_TEST_EXPECT_STRING(text, ==,
                          "0009121b242d363f48515a636c757e879099a2abb4bdc6cfd8e1eaf3fc050e17");
}

/* -------------------------------------------------------------------------
 * The cache on disk.
 * ------------------------------------------------------------------------- */

#define PATH_CAPACITY ((size_t)32768)

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

/* A cache in a new directory of its own, which remove_cache() removes. */
typedef struct cache_fixture
{
    char root[PATH_CAPACITY];
    char directory[PATH_CAPACITY];
    tb_shader_cache *cache;
} cache_fixture;

static void join(const char *first, const char *second, char *out)
{
    size_t required = 0;
    TB_TEST_REQUIRE_INT(tb_path_join(tb_fs_native_style(), view(first), view(second), out,
                                     PATH_CAPACITY, &required),
                        ==, TB_PATH_OK);
}

static void open_cache(cache_fixture *fixture)
{
    TB_TEST_REQUIRE_INT(tb_fs_temp_directory((tb_path_view){0}, fixture->root, PATH_CAPACITY), ==,
                        TB_FS_OK);
    /* The cache makes its directory, and the parents it lacks. */
    join(fixture->root, "Saved/ShaderCache", fixture->directory);
    TB_TEST_REQUIRE_INT(tb_shader_cache_open(fixture->directory, &fixture->cache), ==,
                        TB_SHADER_CACHE_OK);
}

/* Remove the cache's directory and everything in it. */
static void remove_cache(cache_fixture *fixture)
{
    tb_directory directory = {0};
    char name[PATH_CAPACITY] = {0};
    char path[PATH_CAPACITY] = {0};
    size_t required = 0;
    tb_shader_cache_close(fixture->cache);
    TB_TEST_REQUIRE_INT(tb_directory_open(view(fixture->directory), &directory), ==, TB_FS_OK);
    while (tb_directory_next(&directory, name, sizeof(name), &required) == TB_FS_OK)
    {
        join(fixture->directory, name, path);
        TB_TEST_EXPECT_INT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    }
    (void)tb_directory_close(&directory);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(fixture->directory)), ==, TB_FS_OK);
    join(fixture->root, "Saved", path);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(fixture->root)), ==, TB_FS_OK);
}

static void entry_path(const cache_fixture *fixture, const tb_shader_key *key, char *path)
{
    char name[TB_SHADER_KEY_TEXT_CAPACITY + 8] = {0};
    tb_shader_key_format(key, name);
    memcpy(name + TB_SHADER_KEY_SIZE * 2, ".tbsc", sizeof(".tbsc"));
    join(fixture->directory, name, path);
}

static tb_shader_key key_numbered(unsigned number)
{
    tb_shader_key shader = {0};
    tb_shader_key key = {0};
    shader.bytes[0] = (uint8_t)number;
    tb_shader_cache_key(&shader, "a compiler", &key);
    return key;
}

static const unsigned char code_a[] = {'D', 'X', 'B', 'C', 1, 2, 3, 4, 5, 6, 7, 8, 9};
static const unsigned char code_b[] = {'D', 'X', 'B', 'C', 9, 8, 7};

TB_TEST(shader_cache, a_put_is_found_by_its_key)
{
    cache_fixture fixture = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_key key = key_numbered(1);
    const tb_shader_key other = key_numbered(2);
    const void *code = NULL;
    size_t size = 0;
    open_cache(&fixture);

    TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                       TB_SHADER_CACHE_MISS);
    TB_TEST_EXPECT_POINTER(code, ==, NULL);
    TB_TEST_EXPECT_UINT(size, ==, 0);

    TB_TEST_REQUIRE_INT(tb_shader_cache_put(fixture.cache, &key, code_a, sizeof(code_a)), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_UINT(size, ==, sizeof(code_a));
    TB_TEST_EXPECT_BYTES(code, code_a, sizeof(code_a));
    /* A DXIL container is read as 32-bit words. */
    TB_TEST_EXPECT_UINT((uintptr_t)code % 8u, ==, 0);
    TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &other, arena, &code, &size), ==,
                       TB_SHADER_CACHE_MISS);

    /* A put replaces what the key had. */
    TB_TEST_REQUIRE_INT(tb_shader_cache_put(fixture.cache, &key, code_b, sizeof(code_b)), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_UINT(size, ==, sizeof(code_b));
    TB_TEST_EXPECT_BYTES(code, code_b, sizeof(code_b));

    /* Another cache on the same directory sees the same entries. */
    tb_shader_cache *again = NULL;
    TB_TEST_REQUIRE_INT(tb_shader_cache_open(fixture.directory, &again), ==, TB_SHADER_CACHE_OK);
    TB_TEST_EXPECT_INT(tb_shader_cache_get(again, &key, arena, &code, &size), ==,
                       TB_SHADER_CACHE_OK);
    tb_shader_cache_close(again);

    /* No code is no entry. */
    TB_TEST_EXPECT_INT(tb_shader_cache_put(fixture.cache, &other, code_a, 0), ==,
                       TB_SHADER_CACHE_FAILED);
    remove_cache(&fixture);
    tb_arena_destroy(arena);
}

/* Write an entry's bytes as they are, for a case to damage. */
static void write_entry(const char *path, const unsigned char *bytes, size_t length)
{
    TB_TEST_REQUIRE_INT(tb_fs_write_file(view(path), bytes, length), ==, TB_FS_OK);
}

TB_TEST(shader_cache, a_damaged_entry_is_a_miss_that_a_put_repairs)
{
    cache_fixture fixture = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_key key = key_numbered(3);
    const tb_shader_key other = key_numbered(4);
    char path[PATH_CAPACITY] = {0};
    char other_path[PATH_CAPACITY] = {0};
    tb_fs_buffer whole = {0};
    const void *code = NULL;
    size_t size = 0;
    open_cache(&fixture);
    entry_path(&fixture, &key, path);
    entry_path(&fixture, &other, other_path);
    TB_TEST_REQUIRE_INT(tb_shader_cache_put(fixture.cache, &key, code_a, sizeof(code_a)), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_INT(tb_fs_read_file(view(path), NULL, 4096, &whole), ==, TB_FS_OK);

    /* Every truncation, from nothing to all but the last byte. */
    for (size_t length = 0; length < whole.length; length++)
    {
        TB_TEST_CONTEXT("cut to %zu bytes", length);
        write_entry(path, whole.data, length);
        TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                           TB_SHADER_CACHE_DAMAGED);
        TB_TEST_EXPECT_POINTER(code, ==, NULL);
    }
    /* Every byte changed, in the header and in the code. */
    for (size_t at = 0; at < whole.length; at++)
    {
        TB_TEST_CONTEXT("byte %zu changed", at);
        whole.data[at] ^= 0x5Au;
        write_entry(path, whole.data, whole.length);
        whole.data[at] ^= 0x5Au;
        TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                           TB_SHADER_CACHE_DAMAGED);
    }
    /* A byte more than the entry says. */
    unsigned char longer[256] = {0};
    TB_TEST_REQUIRE_UINT(whole.length + 1, <=, sizeof(longer));
    memcpy(longer, whole.data, whole.length);
    write_entry(path, longer, whole.length + 1);
    TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                       TB_SHADER_CACHE_DAMAGED);
    /* A whole entry under another key's name is not that key's. */
    write_entry(other_path, whole.data, whole.length);
    TB_TEST_EXPECT_INT(tb_shader_cache_get(fixture.cache, &other, arena, &code, &size), ==,
                       TB_SHADER_CACHE_DAMAGED);

    /* A put replaces a damaged entry, as a caller does after compiling. */
    TB_TEST_REQUIRE_INT(tb_shader_cache_put(fixture.cache, &key, code_a, sizeof(code_a)), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_REQUIRE_INT(tb_shader_cache_get(fixture.cache, &key, arena, &code, &size), ==,
                        TB_SHADER_CACHE_OK);
    TB_TEST_EXPECT_BYTES(code, code_a, sizeof(code_a));
    tb_fs_buffer_free(&whole);
    remove_cache(&fixture);
    tb_arena_destroy(arena);
}

TB_TEST(shader_cache, a_cache_cannot_be_made_under_a_file)
{
    char root[PATH_CAPACITY] = {0};
    char file[PATH_CAPACITY] = {0};
    char below[PATH_CAPACITY] = {0};
    tb_shader_cache *cache = NULL;
    TB_TEST_REQUIRE_INT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    join(root, "file", file);
    join(file, "cache", below);
    TB_TEST_REQUIRE_INT(tb_fs_write_file(view(file), "x", 1), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_shader_cache_open(below, &cache), ==, TB_SHADER_CACHE_FAILED);
    TB_TEST_EXPECT_POINTER(cache, ==, NULL);
    TB_TEST_EXPECT_INT(tb_shader_cache_open("", &cache), ==, TB_SHADER_CACHE_FAILED);
    tb_shader_cache_close(NULL);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(file)), ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

/* -------------------------------------------------------------------------
 * Sharing a cache.
 * ------------------------------------------------------------------------- */

#define SHARING_THREADS 4u
#define SHARING_ROUNDS 40u

typedef struct sharer
{
    tb_shader_cache *cache;
    unsigned number;
    size_t hits;
    size_t wrong; /* Gets that gave other bytes, or called an entry damaged. */
} sharer;

/* Each thread puts and gets two keys that every thread shares, each always
 * with its own code, as compiles of one shader on several threads do. */
static void share(void *argument)
{
    sharer *const self = argument;
    tb_arena *const arena = tb_arena_create(0);
    const tb_shader_key keys[2] = {key_numbered(10), key_numbered(11)};
    const unsigned char *const codes[2] = {code_a, code_b};
    const size_t sizes[2] = {sizeof(code_a), sizeof(code_b)};
    for (unsigned round = 0; round < SHARING_ROUNDS; round++)
    {
        const size_t which = (round + self->number) % 2u;
        const void *code = NULL;
        size_t size = 0;
        /* A put may lose a race to rename on some systems; that is a slower
         * cache, never a wrong one. */
        (void)tb_shader_cache_put(self->cache, &keys[which], codes[which], sizes[which]);
        const tb_shader_cache_result got =
            tb_shader_cache_get(self->cache, &keys[1 - which], arena, &code, &size);
        if (got == TB_SHADER_CACHE_OK)
        {
            self->hits++;
            self->wrong += size != sizes[1 - which] || memcmp(code, codes[1 - which], size) != 0;
        }
        self->wrong += got == TB_SHADER_CACHE_DAMAGED;
        tb_arena_reset(arena);
    }
    tb_arena_destroy(arena);
}

TB_TEST(shader_cache, threads_share_a_cache_and_see_entries_whole)
{
    cache_fixture fixture = {0};
    tb_thread threads[SHARING_THREADS] = {{0}};
    sharer sharers[SHARING_THREADS] = {{0}};
    open_cache(&fixture);
    for (unsigned index = 0; index < SHARING_THREADS; index++)
    {
        sharers[index].cache = fixture.cache;
        sharers[index].number = index;
        tb_thread_create(&threads[index], share, &sharers[index]);
    }
    size_t hits = 0;
    for (unsigned index = 0; index < SHARING_THREADS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(sharers[index].wrong, ==, 0);
        hits += sharers[index].hits;
    }
    TB_TEST_EXPECT_UINT(hits, >, 0);
    remove_cache(&fixture);
}

TB_TEST(shader_cache, results_have_names)
{
    TB_TEST_EXPECT_STRING(tb_shader_cache_result_name(TB_SHADER_CACHE_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_shader_cache_result_name(TB_SHADER_CACHE_MISS), ==, "miss");
    TB_TEST_EXPECT_STRING(tb_shader_cache_result_name(TB_SHADER_CACHE_DAMAGED), ==, "damaged");
    TB_TEST_EXPECT_STRING(tb_shader_cache_result_name(TB_SHADER_CACHE_FAILED), ==, "failed");
    TB_TEST_EXPECT_STRING(tb_shader_cache_result_name((tb_shader_cache_result)99), ==,
                          "unknown result");
}
