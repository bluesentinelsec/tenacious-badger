#include "badger/assert.h"
#include "badger/fs.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "tb_test.h"
#include "test_allocator.h"
#include <string.h>

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}
static void make_fixture(char *root, size_t root_capacity, char *path, size_t path_capacity)
{
    size_t required = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, root_capacity), ==,
                         TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("payload.bin"), path,
                                      path_capacity, &required),
                         ==, TB_PATH_OK);
}
TB_TEST(core_files, whole_file_roundtrips_limits_and_allocator_ownership)
{
    char root[32768] = {0};
    char path[32768] = {0};
    unsigned char payload[8193] = {0};
    tb_test_allocator tracker = {0};
    tb_allocator allocator = {0};
    allocator = tb_test_allocator_start(&tracker);
    make_fixture(root, sizeof(root), path, sizeof(path));
    tb_test_random_bytes(payload, sizeof(payload));
    for (size_t trial = {0}; trial < 24; trial++)
    {
        size_t length = {0};
        tb_fs_buffer content = {0};
        length = trial == 0 ? 0 : (size_t)tb_test_random_below(sizeof(payload));
        TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), payload, length), ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(path), &allocator, length, &content), ==,
                             TB_FS_OK);
        TB_TEST_EXPECT_UINT(content.length, ==, length);
        TB_TEST_EXPECT(memcmp(content.data, payload, length) == 0);
        TB_TEST_EXPECT_UINT(content.data[length], ==, 0);
        TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, content.data, content.capacity));
        tb_fs_buffer_free(&content);
        tb_fs_buffer_free(&content);
        TB_TEST_EXPECT_POINTER(content.data, ==, NULL);
        if (length != 0)
        {
            TB_TEST_EXPECT_UINT(tb_fs_read_file(view(path), &allocator, length - 1, &content), ==,
                                TB_FS_TOO_LARGE);
            TB_TEST_EXPECT_POINTER(content.data, ==, NULL);
        }
    }
    {
        tb_fs_buffer content = {0};
        tb_test_allocator_fail_after(&tracker, 0);
        TB_TEST_EXPECT_UINT(tb_fs_read_file(view(path), &allocator, SIZE_MAX, &content), ==,
                            TB_FS_NO_MEMORY);
        TB_TEST_EXPECT_POINTER(content.data, ==, NULL);
        TB_TEST_EXPECT_UINT(content.capacity, ==, 0);
    }
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

typedef struct growing_file
{
    tb_allocator delegate;
    const char *path;
    bool changed;
    tb_fs_result result;
} growing_file;
static void *grow_during_allocation(void *context, void *memory, size_t old_size, size_t new_size,
                                    size_t alignment)
{
    growing_file *state = NULL;
    state = context;
    if (!state->changed && new_size != 0)
    {
        state->changed = true;
        state->result = tb_fs_write_file(view(state->path), "longer file data", 16);
    }
    return state->delegate.function(state->delegate.context, memory, old_size, new_size, alignment);
}
TB_TEST(core_files, growth_after_size_query_obeys_limit_and_cleans_failed_reallocation)
{
    char root[32768] = {0};
    char path[32768] = {0};
    make_fixture(root, sizeof(root), path, sizeof(path));
    for (size_t scenario = {0}; scenario < 3; scenario++)
    {
        tb_test_allocator tracker = {0};
        growing_file growing = {0};
        tb_allocator allocator = {0};
        tb_fs_buffer content = {0};
        growing.delegate = tb_test_allocator_start(&tracker);
        growing.path = path;
        allocator.function = grow_during_allocation;
        allocator.context = &growing;
        if (scenario == 2)
        {
            tb_test_allocator_fail_after(&tracker, 1);
        }
        TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), "x", 1), ==, TB_FS_OK);
        if (scenario == 0)
        {
            TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(path), &allocator, 100, &content), ==,
                                 TB_FS_OK);
            TB_TEST_EXPECT_UINT(content.length, ==, 16);
            TB_TEST_EXPECT(memcmp(content.data, "longer file data", 16) == 0);
        }
        else
        {
            TB_TEST_EXPECT_UINT(
                tb_fs_read_file(view(path), &allocator, scenario == 1 ? 8 : 100, &content), ==,
                scenario == 1 ? TB_FS_TOO_LARGE : TB_FS_NO_MEMORY);
            TB_TEST_EXPECT_POINTER(content.data, ==, NULL);
        }
        TB_TEST_EXPECT_UINT(growing.result, ==, TB_FS_OK);
        tb_fs_buffer_free(&content);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
        TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}
TB_TEST(core_files, directory_creation_and_replacement_report_bad_inputs)
{
    char root[32768] = {0};
    char path[32768] = {0};
    char nested[32768] = {0};
    size_t required = {0};
    tb_fs_buffer content = {0};
    make_fixture(root, sizeof(root), path, sizeof(path));
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("a/b"), nested,
                                      sizeof(nested), &required),
                         ==, TB_PATH_OK);
    TB_TEST_EXPECT_UINT(tb_fs_make_directories(view(nested)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_make_directories(view(nested)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(nested)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(
        tb_path_parent(tb_fs_native_style(), view(nested), nested, sizeof(nested), &required), ==,
        TB_PATH_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(nested)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view(path), "new", 3), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_make_directories(view(path)), ==, TB_FS_NOT_DIRECTORY);
    TB_TEST_EXPECT_UINT(tb_fs_read_file(view(root), NULL, 100, &content), ==, TB_FS_IS_DIRECTORY);
    TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(path), NULL, 100, &content), ==, TB_FS_OK);
    TB_TEST_EXPECT_STRING((const char *)content.data, ==, "new");
    tb_fs_buffer_free(&content);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view(root), "new", 3), !=, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view("."), "new", 3), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view(".."), "new", 3), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view(""), "new", 3), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_replace_file(view("bad\xFF"), "new", 3), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_make_directories(view("bad\xFF")), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_make_directories(view("")), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_read_file(view(path), NULL, 100, &content), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

typedef struct replacement_reader
{
    const char *path;
    tb_platform_atomic_u32 stop;
    tb_mutex start_lock;
    tb_condition ready;
    bool started;
    size_t failures;
    size_t reads;
} replacement_reader;
static void read_during_replacement(void *context)
{
    replacement_reader *reader = NULL;
    reader = context;
    while (tb_platform_atomic_u32_load(&reader->stop) == 0)
    {
        tb_fs_buffer content = {0};
        tb_fs_result result = {0};
        result = tb_fs_read_file(view(reader->path), NULL, 4096, &content);
        if (result != TB_FS_OK || content.length != 4096)
        {
            reader->failures++;
        }
        else
        {
            unsigned char first = {0};
            first = content.data[0];
            if (first != 'a' && first != 'b')
            {
                reader->failures++;
            }
            for (size_t index = {0}; index < content.length; index++)
            {
                if (content.data[index] != first)
                {
                    reader->failures++;
                    break;
                }
            }
        }
        tb_fs_buffer_free(&content);
        reader->reads++;
        tb_mutex_lock(&reader->start_lock);
        reader->started = true;
        tb_condition_wake_one(&reader->ready);
        tb_mutex_unlock(&reader->start_lock);
    }
}
TB_TEST(core_files, concurrent_readers_never_observe_partial_atomic_replacements)
{
    char root[32768] = {0};
    char path[32768] = {0};
    char payload[4096] = {0};
    replacement_reader reader = {0};
    tb_thread thread = {0};
    size_t failures = {0};
    make_fixture(root, sizeof(root), path, sizeof(path));
    memset(payload, 'a', sizeof(payload));
    TB_TEST_REQUIRE_UINT(tb_fs_replace_file(view(path), payload, sizeof(payload)), ==, TB_FS_OK);
    reader.path = path;
    tb_thread_create(&thread, read_during_replacement, &reader);
    tb_mutex_lock(&reader.start_lock);
    while (!reader.started)
    {
        tb_condition_wait(&reader.ready, &reader.start_lock);
    }
    tb_mutex_unlock(&reader.start_lock);
    for (size_t round = {0}; round < 200; round++)
    {
        memset(payload, round % 2 == 0 ? 'b' : 'a', sizeof(payload));
        if (tb_fs_replace_file(view(path), payload, sizeof(payload)) != TB_FS_OK)
        {
            failures++;
        }
    }
    tb_platform_atomic_u32_store(&reader.stop, 1);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_UINT(failures, ==, 0);
    TB_TEST_EXPECT_UINT(reader.failures, ==, 0);
    TB_TEST_EXPECT(reader.reads > 0);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

static void invalid_file_helper_contract(void *context)
{
    size_t choice = {0};
    tb_fs_buffer buffer = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        (void)tb_fs_read_file(view("x"), NULL, 1, NULL);
        break;
    case 1:
        buffer.length = 1;
        (void)tb_fs_read_file(view("x"), NULL, 1, &buffer);
        break;
    case 2:
        tb_fs_buffer_free(NULL);
        break;
    case 3:
        (void)tb_fs_write_file(view("x"), NULL, 1);
        break;
    case 4:
        (void)tb_fs_replace_file(view("x"), NULL, 1);
        break;
    case 5:
        (void)tb_fs_make_directories((tb_path_view){NULL, 1});
        break;
    default:
        break;
    }
}
TB_TEST(core_files, helper_required_pointers_and_empty_output_are_contracts)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 6; choice++)
    {
        TB_TEST_EXPECT_TRAP(invalid_file_helper_contract, &choice, "assertion failed");
    }
}
