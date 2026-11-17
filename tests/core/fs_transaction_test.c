#include "badger/str.h"
#include "core/fs_internal.h"
#include "tb_test.h"
#include <string.h>

static size_t failure_stage = {0};
static size_t write_calls = {0};
static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}
static tb_fs_result injected_write(tb_file *file, const void *bytes, size_t length, size_t *count)
{
    write_calls++;
    *count = 0;
    if (failure_stage == 1)
    {
        return TB_FS_OK;
    }
    if (failure_stage == 2 && write_calls == 2)
    {
        return TB_FS_NO_SPACE;
    }
    return tb_file_write(file, bytes, length > 3 ? 3 : length, count);
}
static tb_fs_result injected_flush(tb_file *file)
{
    return failure_stage == 3 ? TB_FS_OS_ERROR : tb_file_flush(file);
}
static tb_fs_result injected_close(tb_file *file)
{
    tb_fs_result result = {0};
    result = tb_file_close(file);
    return failure_stage == 4 ? TB_FS_OS_ERROR : result;
}
static tb_fs_result injected_rename(tb_path_view source, tb_path_view destination)
{
    return failure_stage == 5 ? TB_FS_ACCESS : tb_fs_rename(source, destination);
}
TB_TEST(core_files_transaction, failure_at_every_step_preserves_old_file_and_removes_temporary)
{
    static const tb_fs_write_operations operations = {injected_write, injected_flush,
                                                      injected_close, injected_rename};
    char root[32768] = {0};
    char path[32768] = {0};
    size_t required = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("target"), path,
                                      sizeof(path), &required),
                         ==, TB_PATH_OK);
    for (size_t stage = {0}; stage < 6; stage++)
    {
        tb_fs_buffer content = {0};
        tb_directory directory = {0};
        char name[64] = {0};
        tb_fs_result result = {0};
        TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), "old", 3), ==, TB_FS_OK);
        failure_stage = stage;
        write_calls = 0;
        result = tb_fs_replace_with_operations(view(path), "replacement", 11, &operations);
        if (stage == 0)
        {
            TB_TEST_EXPECT_UINT(result, ==, TB_FS_OK);
            TB_TEST_EXPECT(write_calls > 1);
        }
        else
        {
            TB_TEST_EXPECT(result != TB_FS_OK);
        }
        TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(path), NULL, 100, &content), ==, TB_FS_OK);
        TB_TEST_EXPECT_STRING((const char *)content.data, ==, stage == 0 ? "replacement" : "old");
        tb_fs_buffer_free(&content);
        TB_TEST_REQUIRE_UINT(tb_directory_open(view(root), &directory), ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_directory_next(&directory, name, sizeof(name), &required), ==,
                             TB_FS_OK);
        TB_TEST_EXPECT_STRING(name, ==, "target");
        TB_TEST_EXPECT_UINT(tb_directory_next(&directory, name, sizeof(name), &required), ==,
                            TB_FS_END);
        TB_TEST_EXPECT_UINT(tb_directory_close(&directory), ==, TB_FS_OK);
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}
