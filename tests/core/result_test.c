/* Every module that reports failure with a result can name the result, for a
 * log or a message. The names are checked as a set: each result has one,
 * no two results of a module share one, and a value that is no result is
 * named as such and not as its neighbour. */
#include "badger/base64.h"
#include "badger/cli.h"
#include "badger/deflate.h"
#include "badger/fs.h"
#include "badger/json.h"
#include "badger/library.h"
#include "badger/log.h"
#include "badger/package.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/tar.h"
#include "badger/vfs.h"
#include "badger/virtual_memory.h"
#include "badger/zip.h"
#include "tb_test.h"

#include <stddef.h>

typedef const char *name_function(int result);

static const char *name_of_base64(int result)
{
    return tb_base64_result_name((tb_base64_result)result);
}

static const char *name_of_cli(int result)
{
    return tb_cli_result_name((tb_cli_result)result);
}

static const char *name_of_deflate(int result)
{
    return tb_deflate_result_name((tb_deflate_result)result);
}

static const char *name_of_json(int result)
{
    return tb_json_result_name((tb_json_result)result);
}

static const char *name_of_fs(int result)
{
    return tb_fs_result_name((tb_fs_result)result);
}

static const char *name_of_library(int result)
{
    return tb_library_result_name((tb_library_result)result);
}

static const char *name_of_log(int result)
{
    return tb_log_result_name((tb_log_result)result);
}

static const char *name_of_package(int result)
{
    return tb_package_result_name((tb_package_result)result);
}

static const char *name_of_path(int result)
{
    return tb_path_result_name((tb_path_result)result);
}

static const char *name_of_stream(int result)
{
    return tb_stream_result_name((tb_stream_result)result);
}

static const char *name_of_tar(int result)
{
    return tb_tar_result_name((tb_tar_result)result);
}

static const char *name_of_vfs(int result)
{
    return tb_vfs_result_name((tb_vfs_result)result);
}

static const char *name_of_vm(int result)
{
    return tb_vm_result_name((tb_vm_result)result);
}

static const char *name_of_zip(int result)
{
    return tb_zip_result_name((tb_zip_result)result);
}

TB_TEST(core_result, every_result_has_a_name_of_its_own)
{
    static const struct
    {
        const char *module;
        name_function *name;
        int last; /* The last result of the enumeration. */
    } rows[] = {
        {"base64", name_of_base64, TB_BASE64_BUFFER_TOO_SMALL},
        {"cli", name_of_cli, TB_CLI_ERROR},
        {"deflate", name_of_deflate, TB_DEFLATE_UNSUPPORTED},
        {"fs", name_of_fs, TB_FS_END},
        {"json", name_of_json, TB_JSON_OUTPUT_FAILED},
        {"library", name_of_library, TB_LIBRARY_NO_MEMORY},
        {"log", name_of_log, TB_LOG_TOO_LARGE},
        {"package", name_of_package, TB_PACKAGE_OUTPUT_FAILED},
        {"path", name_of_path, TB_PATH_TOO_LARGE},
        {"stream", name_of_stream, TB_STREAM_OS_ERROR},
        {"tar", name_of_tar, TB_TAR_OUTPUT_FAILED},
        {"vfs", name_of_vfs, TB_VFS_INPUT_FAILED},
        {"vm", name_of_vm, TB_VM_OS_ERROR},
        {"zip", name_of_zip, TB_ZIP_OUTPUT_FAILED},
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
    TB_TEST_EXPECT_STRING(tb_library_result_name(TB_LIBRARY_NO_MEMORY), ==, "out of memory");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_path_result_name(TB_PATH_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_log_result_name(TB_LOG_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_OS_ERROR), ==, "operating system error");
    TB_TEST_EXPECT_STRING(tb_vm_result_name(TB_VM_OS_ERROR), ==, "operating system error");
    TB_TEST_EXPECT_STRING(tb_fs_result_name(TB_FS_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_vm_result_name(TB_VM_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_log_result_name(TB_LOG_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_library_result_name(TB_LIBRARY_INVALID), ==, "invalid argument");
}
