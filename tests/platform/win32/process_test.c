/* The Windows side of badger/process.h: a command line in UTF-16, split as
 * the system splits it, and converted to UTF-8, including what the ANSI
 * code page cannot hold. */
#include "badger/process.h"

#include "process_arguments.h"
#include "tb_test.h"

TB_TEST(platform_process_win32, a_command_line_is_split_and_converted_to_utf8)
{
    /* The program; "café"; "two words", quoted; a\"b, an escaped quote;
     * U+1F600, outside the Basic Multilingual Plane; an empty argument. */
    static const WCHAR line[] = L"C:\\Games\\burrow.exe caf\u00e9 \"two words\" a\\\"b "
                                L"\xD83D\xDE00 \"\"";
    static const char *const expected[] = {
        "C:\\Games\\burrow.exe", "caf\xC3\xA9", "two words", "a\"b", "\xF0\x9F\x98\x80", "",
    };
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    TB_TEST_REQUIRE(tb_win32_arguments_from(line, &arguments));
    TB_TEST_REQUIRE_UINT(arguments.count, ==, TB_TEST_COUNT_OF(expected));
    for (size_t index = 0; index < arguments.count; index++)
    {
        TB_TEST_CONTEXT("argument %zu", index);
        TB_TEST_EXPECT_STRING(arguments.values[index], ==, expected[index]);
    }
    tb_process_release_arguments(&arguments);
}

/* A lone surrogate is not text; it becomes U+FFFD rather than invalid
 * UTF-8. */
TB_TEST(platform_process_win32, an_unpaired_surrogate_becomes_the_replacement_character)
{
    static const WCHAR line[] = L"program x\xD800y";
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    TB_TEST_REQUIRE(tb_win32_arguments_from(line, &arguments));
    TB_TEST_REQUIRE_UINT(arguments.count, ==, 2);
    TB_TEST_EXPECT_STRING(arguments.values[1], ==, "x\xEF\xBF\xBDy");
    tb_process_release_arguments(&arguments);
}

/* The process's own command line holds this executable's name. */
TB_TEST(platform_process_win32, the_processes_arguments_come_from_the_system)
{
    char program[] = "ignored";
    char *values[] = {program, NULL};
    tb_arguments arguments = TB_ARGUMENTS_INIT;

    TB_TEST_REQUIRE(tb_process_get_arguments(1, values, &arguments));
    TB_TEST_REQUIRE_UINT(arguments.count, >=, 1);
    TB_TEST_EXPECT_STRING(arguments.values[0], !=, "ignored");
    tb_process_release_arguments(&arguments);
}
