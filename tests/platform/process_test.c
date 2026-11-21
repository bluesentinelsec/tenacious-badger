/* The process's arguments, on every platform: at least the program, each
 * one terminated, and nothing left once they are released. Linux and macOS
 * give back main's own; Windows reads the real command line, whose
 * conversion tests/platform/win32/process_test.c checks. */
#include "badger/process.h"

#include "fs.h"
#include "tb_test.h"

#include <stddef.h>

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

TB_TEST(platform_process, the_arguments_hold_the_program_and_are_utf8)
{
    char program[] = "tb_platform_tests";
    char first[] = "first";
    char *values[] = {program, first, NULL};
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    TB_TEST_REQUIRE(tb_process_get_arguments(2, values, &arguments));
    TB_TEST_REQUIRE_UINT(arguments.count, >=, 1);
    TB_TEST_REQUIRE(arguments.values != NULL);
    for (size_t index = 0; index < arguments.count; index++)
    {
        TB_TEST_CONTEXT("argument %zu", index);
        TB_TEST_REQUIRE(arguments.values[index] != NULL);
        TB_TEST_EXPECT(
            tb_native_fs_text_valid(arguments.values[index], text_length(arguments.values[index])));
    }
    tb_process_release_arguments(&arguments);
    TB_TEST_EXPECT_UINT(arguments.count, ==, 0);
    TB_TEST_EXPECT(arguments.values == NULL && arguments.storage == NULL);
}

TB_TEST(platform_process, releasing_zero_arguments_does_nothing)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;
    tb_process_release_arguments(&arguments);
    TB_TEST_EXPECT_UINT(arguments.count, ==, 0);
}
