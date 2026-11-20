/* On Windows, tb_crash_write_dumps() takes an existing directory, or the
 * system's temporary one, and refuses anything else without changing what
 * it had. The dump itself is written only by a crash, which the crash probe
 * makes (tests/ExpectCrashReport.cmake). */
#include "badger/crash.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string.h>

TB_TEST(platform_crash_dump, an_existing_directory_or_the_temporary_one_is_taken)
{
    TB_TEST_EXPECT(tb_crash_write_dumps(NULL));
    TB_TEST_EXPECT(tb_crash_write_dumps("."));
    TB_TEST_EXPECT(tb_crash_write_dumps("./"));
}

TB_TEST(platform_crash_dump, anything_but_an_existing_directory_is_refused)
{
    char system_file[MAX_PATH + 32] = {0};
    const UINT length = GetSystemDirectoryA(system_file, MAX_PATH);

    TB_TEST_EXPECT(!tb_crash_write_dumps("tb-crash-dump-test-no-such-directory"));
    TB_TEST_EXPECT(!tb_crash_write_dumps("\xff\xfe"));
    TB_TEST_EXPECT(!tb_crash_write_dumps(""));
    /* A file that every Windows has, which is not a directory. */
    TB_TEST_REQUIRE(length > 0 && length < MAX_PATH);
    (void)memcpy(system_file + length, "\\kernel32.dll", sizeof("\\kernel32.dll"));
    TB_TEST_EXPECT(!tb_crash_write_dumps(system_file));
}
