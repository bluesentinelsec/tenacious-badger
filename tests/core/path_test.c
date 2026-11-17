#include "badger/assert.h"
#include "badger/path.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"
#include <string.h>

static tb_path_view view(const char *text)
{
    tb_path_view result = {0};
    result.data = text;
    result.length = tb_str_length(text);
    return result;
}

TB_TEST(core_path, normalization_covers_roots_and_dot_components)
{
    static const struct
    {
        tb_path_style style;
        const char *input;
        const char *expected;
    } rows[] = {{TB_PATH_POSIX, "", "."},
                {TB_PATH_POSIX, ".", "."},
                {TB_PATH_POSIX, "..", ".."},
                {TB_PATH_POSIX, "/", "/"},
                {TB_PATH_POSIX, "//", "//"},
                {TB_PATH_POSIX, "///a//b/", "/a/b"},
                {TB_PATH_POSIX, "a/./b", "a/b"},
                {TB_PATH_POSIX, "a/../b", "b"},
                {TB_PATH_POSIX, "../a/../../b", "../../b"},
                {TB_PATH_POSIX, "/../../a", "/a"},
                {TB_PATH_POSIX, "a/..", "."},
                {TB_PATH_POSIX, "a/b/..", "a"},
                {TB_PATH_POSIX, "a\\b", "a\\b"},
                {TB_PATH_POSIX, "//host/a/../b", "//host/b"},
                {TB_PATH_WINDOWS, "C:", "C:"},
                {TB_PATH_WINDOWS, "C:a", "C:a"},
                {TB_PATH_WINDOWS, "C:a/..", "C:"},
                {TB_PATH_WINDOWS, "C:/", "C:/"},
                {TB_PATH_WINDOWS, "C:\\a\\..\\b\\", "C:/b"},
                {TB_PATH_WINDOWS, "\\a\\..\\b", "/b"},
                {TB_PATH_WINDOWS, "\\\\server\\share", "//server/share/"},
                {TB_PATH_WINDOWS, "\\\\server\\share\\a\\..\\..", "//server/share/"},
                {TB_PATH_WINDOWS, "\\\\?\\C:\\a\\..\\b", "//?/C:/a/../b"},
                {TB_PATH_WINDOWS, "\\\\?\\UNC\\server\\share\\a", "//?/UNC/server/share/a"},
                {TB_PATH_POSIX, "dir/\xE6\x97\xA5\xF0\x9F\x98\x80/../x", "dir/x"}};
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(rows); row++)
    {
        char output[256] = {0};
        char again[256] = {0};
        size_t required = {0};
        tb_path_result result = {0};
        char *owned = NULL;
        TB_TEST_CONTEXT("path %s", rows[row].input);
        TB_TEST_REQUIRE_UINT(tb_path_normalize(rows[row].style, view(rows[row].input), output,
                                               sizeof(output), &required),
                             ==, TB_PATH_OK);
        TB_TEST_EXPECT_STRING(output, ==, rows[row].expected);
        TB_TEST_EXPECT_UINT(required, ==, tb_str_length(output) + 1);
        TB_TEST_REQUIRE_UINT(
            tb_path_normalize(rows[row].style, view(output), again, sizeof(again), &required), ==,
            TB_PATH_OK);
        TB_TEST_EXPECT_STRING(again, ==, output);
        owned = tb_path_normalize_new(NULL, rows[row].style, view(rows[row].input), &result);
        TB_TEST_EXPECT_UINT(result, ==, TB_PATH_OK);
        TB_TEST_EXPECT_STRING(owned, ==, output);
        tb_str_free(NULL, owned);
        for (size_t capacity = {0}; capacity < required; capacity++)
        {
            memset(output, 0x55, sizeof(output));
            TB_TEST_EXPECT_UINT(tb_path_normalize(rows[row].style, view(rows[row].input), output,
                                                  capacity, &required),
                                ==, TB_PATH_NO_SPACE);
            for (size_t byte = {0}; byte < sizeof(output); byte++)
            {
                TB_TEST_REQUIRE_INT(output[byte], ==, 0x55);
            }
        }
    }
}

TB_TEST(core_path, parsing_returns_borrowed_parts_without_changing_spelling)
{
    static const struct
    {
        const char *path;
        const char *name;
        const char *stem;
        const char *extension;
    } rows[] = {{"/a/file.tar.gz", "file.tar.gz", "file.tar", ".gz"},
                {".bashrc", ".bashrc", ".bashrc", ""},
                {"name.", "name.", "name", "."},
                {"..", "..", "..", ""},
                {".", ".", ".", ""},
                {"/a/", "", "", ""},
                {"/", "", "", ""},
                {"", "", "", ""}};
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_path_parts parts = {0};
        tb_path_view input = {0};
        input = view(rows[row].path);
        TB_TEST_REQUIRE_UINT(tb_path_parse(TB_PATH_POSIX, input, &parts), ==, TB_PATH_OK);
        TB_TEST_EXPECT_UINT(parts.name.length, ==, tb_str_length(rows[row].name));
        TB_TEST_EXPECT(memcmp(parts.name.data, rows[row].name, parts.name.length) == 0);
        TB_TEST_EXPECT_UINT(parts.stem.length, ==, tb_str_length(rows[row].stem));
        TB_TEST_EXPECT(memcmp(parts.stem.data, rows[row].stem, parts.stem.length) == 0);
        TB_TEST_EXPECT_UINT(parts.extension.length, ==, tb_str_length(rows[row].extension));
        TB_TEST_EXPECT(memcmp(parts.extension.data, rows[row].extension, parts.extension.length) ==
                       0);
        TB_TEST_EXPECT_UINT(parts.directory.length + parts.name.length, ==, input.length);
        TB_TEST_EXPECT_POINTER(parts.name.data, ==, input.data + parts.directory.length);
    }
    {
        tb_path_parts parts = {0};
        TB_TEST_REQUIRE_UINT(tb_path_parse(TB_PATH_WINDOWS, view("C:foo"), &parts), ==, TB_PATH_OK);
        TB_TEST_EXPECT(!parts.absolute);
        TB_TEST_EXPECT_UINT(parts.root.length, ==, 2);
        TB_TEST_REQUIRE_UINT(tb_path_parse(TB_PATH_WINDOWS, view("/foo"), &parts), ==, TB_PATH_OK);
        TB_TEST_EXPECT(!parts.absolute);
        TB_TEST_EXPECT_UINT(parts.root.length, ==, 1);
        TB_TEST_REQUIRE_UINT(tb_path_parse(TB_PATH_WINDOWS, view("//?/UNC/server/share/a"), &parts),
                             ==, TB_PATH_OK);
        TB_TEST_EXPECT(parts.absolute);
        TB_TEST_EXPECT(parts.verbatim);
    }
}

TB_TEST(core_path, joins_relatives_extensions_and_parents_compose)
{
    char output[256] = {0};
    size_t required = {0};
    tb_path_result result = {0};
    char *owned = NULL;
    TB_TEST_EXPECT_UINT(
        tb_path_join(TB_PATH_POSIX, view("/a/b"), view("../c"), output, sizeof(output), &required),
        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "/a/c");
    TB_TEST_EXPECT_UINT(tb_path_join(TB_PATH_WINDOWS, view("C:/a"), view("D:/b"), output,
                                     sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "D:/b");
    TB_TEST_EXPECT_UINT(
        tb_path_join(TB_PATH_WINDOWS, view("C:"), view("b"), output, sizeof(output), &required), ==,
        TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "C:b");
    TB_TEST_EXPECT_UINT(tb_path_relative(TB_PATH_POSIX, view("/a/b"), view("/a/c/d"), output,
                                         sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "../c/d");
    TB_TEST_EXPECT_UINT(
        tb_path_relative(TB_PATH_POSIX, view("/a/b"), view("/"), output, sizeof(output), &required),
        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "../..");
    TB_TEST_EXPECT_UINT(
        tb_path_relative(TB_PATH_POSIX, view("/a"), view("/a"), output, sizeof(output), &required),
        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, ".");
    TB_TEST_EXPECT_UINT(tb_path_relative(TB_PATH_WINDOWS, view("C:/a"), view("D:/b"), output,
                                         sizeof(output), &required),
                        ==, TB_PATH_DIFFERENT_ROOT);
    TB_TEST_EXPECT_UINT(
        tb_path_relative(TB_PATH_POSIX, view("a"), view("b"), output, sizeof(output), &required),
        ==, TB_PATH_DIFFERENT_ROOT);
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view("a.tar.gz"), view(".zip"),
                                                  output, sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "a.tar.zip");
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view(".bashrc"), view(".old"),
                                                  output, sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, ".bashrc.old");
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view("a.txt"), view(""), output,
                                                  sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "a");
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view("a/"), view(".x"), output,
                                                  sizeof(output), &required),
                        ==, TB_PATH_INVALID);
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view("a"), view("x"), output,
                                                  sizeof(output), &required),
                        ==, TB_PATH_INVALID);
    TB_TEST_EXPECT_UINT(tb_path_replace_extension(TB_PATH_POSIX, view("a"), view(".x/y"), output,
                                                  sizeof(output), &required),
                        ==, TB_PATH_INVALID);
    TB_TEST_EXPECT_UINT(
        tb_path_parent(TB_PATH_POSIX, view("/a/b/"), output, sizeof(output), &required), ==,
        TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "/a");
    TB_TEST_EXPECT_UINT(tb_path_parent(TB_PATH_POSIX, view("a"), output, sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, ".");
    TB_TEST_EXPECT_UINT(
        tb_path_parent(TB_PATH_WINDOWS, view("//host/share/"), output, sizeof(output), &required),
        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "//host/share/");
    TB_TEST_EXPECT_UINT(
        tb_path_to_native(TB_PATH_WINDOWS, view("C:/a/b"), output, sizeof(output), &required), ==,
        TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "C:\\a\\b");
    owned = tb_path_join_new(NULL, TB_PATH_POSIX, view("/a"), view("b"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "/a/b");
    tb_str_free(NULL, owned);
    owned = tb_path_rooted_join_new(NULL, TB_PATH_POSIX, view("/a"), view("b"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "/a/b");
    tb_str_free(NULL, owned);
    owned = tb_path_relative_new(NULL, TB_PATH_POSIX, view("/a"), view("/b"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "../b");
    tb_str_free(NULL, owned);
    owned = tb_path_replace_extension_new(NULL, TB_PATH_POSIX, view("a.x"), view(".y"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "a.y");
    tb_str_free(NULL, owned);
    owned = tb_path_parent_new(NULL, TB_PATH_POSIX, view("/a"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "/");
    tb_str_free(NULL, owned);
    owned = tb_path_to_native_new(NULL, TB_PATH_POSIX, view("/a"), &result);
    TB_TEST_EXPECT_STRING(owned, ==, "/a");
    tb_str_free(NULL, owned);
}

TB_TEST(core_path, rooted_joins_refuse_lexical_escape)
{
    static const char *const bad[] = {"../a",        "a/../../b", "/absolute",         "C:relative",
                                      "C:/absolute", "\\rooted",  "\\\\host\\share\\x"};
    char output[128] = {0};
    size_t required = {0};
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(bad); index++)
    {
        output[0] = 42;
        TB_TEST_EXPECT_UINT(tb_path_rooted_join(TB_PATH_WINDOWS, view("C:/root"), view(bad[index]),
                                                output, sizeof(output), &required),
                            ==, TB_PATH_ESCAPE);
        TB_TEST_EXPECT_INT(output[0], ==, 42);
        TB_TEST_EXPECT_UINT(required, ==, 0);
    }
    TB_TEST_EXPECT_UINT(tb_path_rooted_join(TB_PATH_POSIX, view("/root"), view("a/../b"), output,
                                            sizeof(output), &required),
                        ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "/root/b");
    TB_TEST_EXPECT_UINT(tb_path_rooted_join(TB_PATH_POSIX, view("root"), view("b"), output,
                                            sizeof(output), &required),
                        ==, TB_PATH_INVALID);
    TB_TEST_EXPECT_UINT(tb_path_rooted_join(TB_PATH_WINDOWS, view("//?/C:/root"), view("b"), output,
                                            sizeof(output), &required),
                        ==, TB_PATH_INVALID);
}

TB_TEST(core_path, malformed_roots_and_text_leave_outputs_untouched)
{
    static const char *const bad[] = {"//server", "//server/",      "//./disk",   "//?/",
                                      "//?/C:",   "//?/other/path", "//../share", "a\xFF"};
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(bad); index++)
    {
        char output[32] = {0};
        size_t required = {0};
        tb_path_parts parts = {0};
        tb_path_result result = {0};
        output[0] = 42;
        parts.root.length = 77;
        TB_TEST_EXPECT_UINT(tb_path_parse(TB_PATH_WINDOWS, view(bad[index]), &parts), ==,
                            TB_PATH_INVALID);
        TB_TEST_EXPECT_UINT(parts.root.length, ==, 77);
        TB_TEST_EXPECT_UINT(
            tb_path_normalize(TB_PATH_WINDOWS, view(bad[index]), output, sizeof(output), &required),
            ==, TB_PATH_INVALID);
        TB_TEST_EXPECT_INT(output[0], ==, 42);
        TB_TEST_EXPECT_UINT(required, ==, 0);
        TB_TEST_EXPECT_POINTER(
            tb_path_normalize_new(NULL, TB_PATH_WINDOWS, view(bad[index]), &result), ==, NULL);
        TB_TEST_EXPECT_UINT(result, ==, TB_PATH_INVALID);
    }
    {
        char output[32] = {0};
        size_t required = {0};
        TB_TEST_EXPECT_UINT(tb_path_normalize(TB_PATH_POSIX, (tb_path_view){"a\0b", 3}, output,
                                              sizeof(output), &required),
                            ==, TB_PATH_INVALID);
        TB_TEST_EXPECT_UINT(
            tb_path_normalize(TB_PATH_POSIX, (tb_path_view){0}, output, sizeof(output), &required),
            ==, TB_PATH_OK);
        TB_TEST_EXPECT_STRING(output, ==, ".");
    }
}

TB_TEST(core_path, random_paths_normalize_idempotently_and_relatives_rejoin)
{
    static const char *const components[] = {"a/", "bb/", "./", "../", "ccc//"};
    for (size_t trial = {0}; trial < 5000; trial++)
    {
        char source[256] = {0};
        char normalized[256] = {0};
        char again[256] = {0};
        char relative[512] = {0};
        char joined[512] = {0};
        size_t used = {0};
        size_t required = {0};
        source[0] = '/';
        used = 1;
        for (size_t part = {0}; part < 20; part++)
        {
            size_t choice = {0};
            size_t length = {0};
            choice = (size_t)tb_test_random_below(TB_TEST_COUNT_OF(components));
            length = tb_str_length(components[choice]);
            memcpy(source + used, components[choice], length);
            used += length;
        }
        TB_TEST_REQUIRE_UINT(tb_path_normalize(TB_PATH_POSIX, view(source), normalized,
                                               sizeof(normalized), &required),
                             ==, TB_PATH_OK);
        TB_TEST_REQUIRE_UINT(
            tb_path_normalize(TB_PATH_POSIX, view(normalized), again, sizeof(again), &required), ==,
            TB_PATH_OK);
        TB_TEST_REQUIRE_STRING(normalized, ==, again);
        TB_TEST_REQUIRE_UINT(tb_path_relative(TB_PATH_POSIX, view("/base/dir"), view(normalized),
                                              relative, sizeof(relative), &required),
                             ==, TB_PATH_OK);
        TB_TEST_REQUIRE_UINT(tb_path_join(TB_PATH_POSIX, view("/base/dir"), view(relative), joined,
                                          sizeof(joined), &required),
                             ==, TB_PATH_OK);
        TB_TEST_REQUIRE_STRING(joined, ==, normalized);
    }
}

TB_TEST(core_path, long_paths_aliasing_and_custom_ownership)
{
    char source[5000] = {0};
    size_t required = {0};
    tb_path_result result = {0};
    char *owned = NULL;
    tb_test_allocator checked = {0};
    tb_allocator allocator = {0};
    memset(source, 'a', sizeof(source) - 1);
    TB_TEST_EXPECT_UINT(tb_path_normalize(TB_PATH_POSIX, view(source), NULL, 0, &required), ==,
                        TB_PATH_OK);
    TB_TEST_EXPECT_UINT(required, ==, sizeof(source));
    TB_TEST_EXPECT_UINT(
        tb_path_normalize(TB_PATH_POSIX, view(source), source, sizeof(source), &required), ==,
        TB_PATH_OK);
    allocator = tb_test_allocator_start(&checked);
    owned = tb_path_normalize_new(&allocator, TB_PATH_POSIX, view(source), &result);
    TB_TEST_REQUIRE_UINT(result, ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(owned, ==, source);
    tb_str_free(&allocator, owned);
    TB_TEST_EXPECT_UINT(checked.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(checked.first_violation, ==, "none");
}

static void invalid_contract(void *context)
{
    size_t choice = {0};
    size_t required = {0};
    tb_path_parts parts = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        (void)tb_path_parse((tb_path_style)99, view("a"), &parts);
        break;
    case 1:
        (void)tb_path_parse(TB_PATH_POSIX, (tb_path_view){NULL, 1}, &parts);
        break;
    case 2:
        (void)tb_path_parse(TB_PATH_POSIX, view("a"), NULL);
        break;
    case 3:
        (void)tb_path_normalize(TB_PATH_POSIX, view("a"), NULL, 1, &required);
        break;
    case 4:
        (void)tb_path_normalize(TB_PATH_POSIX, view("a"), NULL, 0, NULL);
        break;
    case 5:
        (void)tb_path_normalize_new(NULL, TB_PATH_POSIX, view("a"), NULL);
        break;
    default:
        break;
    }
}
TB_TEST(core_path, required_pointers_and_style_are_contracts)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 6; choice++)
    {
        TB_TEST_EXPECT_TRAP(invalid_contract, &choice, "assertion failed");
    }
}

/* Each bounded builder must preserve every byte on a short destination. */
TB_TEST(core_path, all_builders_measure_and_preserve_short_buffers)
{
    for (size_t operation = {0}; operation < 7; operation++)
    {
        size_t expected = {0};
        for (size_t capacity = {0}; capacity < 40; capacity++)
        {
            char output[40] = {0};
            size_t required = {0};
            tb_path_result status = {0};
            char *destination = NULL;
            memset(output, 0x55, sizeof(output));
            if (capacity != 0)
            {
                destination = output;
            }
            switch (operation)
            {
            case 0:
                status = tb_path_normalize(TB_PATH_POSIX, view("/root/a/../file"), destination,
                                           capacity, &required);
                break;
            case 1:
                status = tb_path_parent(TB_PATH_POSIX, view("/root/a/file"), destination, capacity,
                                        &required);
                break;
            case 2:
                status = tb_path_to_native(TB_PATH_WINDOWS, view("C:/root/file"), destination,
                                           capacity, &required);
                break;
            case 3:
                status = tb_path_join(TB_PATH_POSIX, view("/root"), view("file"), destination,
                                      capacity, &required);
                break;
            case 4:
                status = tb_path_rooted_join(TB_PATH_POSIX, view("/root"), view("a/../file"),
                                             destination, capacity, &required);
                break;
            case 5:
                status = tb_path_relative(TB_PATH_POSIX, view("/root/a"), view("/root/b/file"),
                                          destination, capacity, &required);
                break;
            case 6:
                status = tb_path_replace_extension(TB_PATH_POSIX, view("/root/file.old"),
                                                   view(".new"), destination, capacity, &required);
                break;
            default:
                break;
            }
            if (capacity == 0)
            {
                expected = required;
            }
            TB_TEST_REQUIRE_UINT(required, ==, expected);
            TB_TEST_REQUIRE_UINT(
                status, ==, capacity == 0 || capacity >= expected ? TB_PATH_OK : TB_PATH_NO_SPACE);
            if (capacity < expected)
            {
                for (size_t byte = {0}; byte < sizeof(output); byte++)
                {
                    TB_TEST_REQUIRE_INT(output[byte], ==, 0x55);
                }
            }
            else
            {
                TB_TEST_REQUIRE_UINT(tb_str_length(output) + 1, ==, expected);
            }
        }
    }
}

TB_TEST(core_path, encoded_names_are_literal_and_split_join_preserves_normalized_paths)
{
    char output[128] = {0};
    size_t required = {0};
    TB_TEST_REQUIRE_UINT(tb_path_rooted_join(TB_PATH_POSIX, view("/root"), view("%2e%2e/%2f"),
                                             output, sizeof(output), &required),
                         ==, TB_PATH_OK);
    TB_TEST_EXPECT_STRING(output, ==, "/root/%2e%2e/%2f");
    for (size_t trial = {0}; trial < 1000; trial++)
    {
        char path[128] = {0};
        size_t length = {0};
        tb_path_parts parts = {0};
        length = 1 + (size_t)tb_test_random_below(100);
        for (size_t index = {0}; index < length; index++)
        {
            path[index] = tb_test_random_below(4) == 0 ? '/' : 'a';
        }
        TB_TEST_REQUIRE_UINT(
            tb_path_normalize(TB_PATH_POSIX, view(path), path, sizeof(path), &required), ==,
            TB_PATH_OK);
        TB_TEST_REQUIRE_UINT(tb_path_parse(TB_PATH_POSIX, view(path), &parts), ==, TB_PATH_OK);
        TB_TEST_REQUIRE_UINT(tb_path_join(TB_PATH_POSIX, parts.directory, parts.name, output,
                                          sizeof(output), &required),
                             ==, TB_PATH_OK);
        TB_TEST_REQUIRE_STRING(output, ==, path);
    }
}
