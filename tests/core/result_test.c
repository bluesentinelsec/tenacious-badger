/* Every module that reports failure with a result can name the result, for a
 * log or a message. The names are checked as a set: each result has one,
 * no two results of a module share one, and a value that is no result is
 * named as such and not as its neighbour. */
#include "badger/fs.h"
#include "badger/log.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/virtual_memory.h"
#include "tb_test.h"

#include <stddef.h>

typedef const char *name_function(int result);

static const char *name_of_fs(int result)
{
    return tb_fs_result_name((tb_fs_result)result);
}

static const char *name_of_log(int result)
{
    return tb_log_result_name((tb_log_result)result);
}

static const char *name_of_path(int result)
{
    return tb_path_result_name((tb_path_result)result);
}

static const char *name_of_vm(int result)
{
    return tb_vm_result_name((tb_vm_result)result);
}

TB_TEST(core_result, every_result_has_a_name_of_its_own)
{
    static const struct
    {
        const char *module;
        name_function *name;
        int last; /* The last result of the enumeration. */
    } rows[] = {
        {"fs", name_of_fs, TB_FS_END},
        {"log", name_of_log, TB_LOG_TOO_LARGE},
        {"path", name_of_path, TB_PATH_TOO_LARGE},
        {"vm", name_of_vm, TB_VM_OS_ERROR},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("%s", rows[row].module);
        /* Success is zero in every module, and is named the same. */
        TB_TEST_EXPECT_STRING(rows[row].name(0), ==, "ok");
        for (int result = 0; result <= rows[row].last; result++)
        {
            const char *const name = rows[row].name(result);

            TB_TEST_REQUIRE_POINTER(name, !=, NULL);
            TB_TEST_EXPECT_UINT(tb_str_length(name), >, 0);
            TB_TEST_EXPECT_STRING(name, !=, "unknown result");
            for (int earlier = 0; earlier < result; earlier++)
            {
                TB_TEST_EXPECT_STRING(name, !=, rows[row].name(earlier));
            }
        }
        TB_TEST_EXPECT_STRING(rows[row].name(rows[row].last + 1), ==, "unknown result");
        TB_TEST_EXPECT_STRING(rows[row].name(-1), ==, "unknown result");
    }
}

/* The conditions that modules share are named the same in each. */
TB_TEST(core_result, shared_conditions_have_the_same_name_in_every_module)
{
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_NOT_FOUND), ==, "not found");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_NO_SPACE), ==, "no space left");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_BUFFER_TOO_SMALL), ==, "buffer too small");
    TB_TEST_EXPECT_STRING(tb_path_result_name(TB_PATH_BUFFER_TOO_SMALL), ==, "buffer too small");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_NO_MEMORY), ==, "out of memory");
    TB_TEST_EXPECT_STRING(tb_vm_result_name(TB_VM_NO_MEMORY), ==, "out of memory");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_path_result_name(TB_PATH_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_log_result_name(TB_LOG_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_OS_ERROR), ==, "operating system error");
    TB_TEST_EXPECT_STRING(tb_vm_result_name(TB_VM_OS_ERROR), ==, "operating system error");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_vm_result_name(TB_VM_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_log_result_name(TB_LOG_INVALID), ==, "invalid argument");
}
