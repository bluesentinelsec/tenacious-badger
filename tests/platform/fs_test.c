#include "badger/assert.h"
#include "badger/file.h"
#include "tb_test.h"
#include <string.h>

static tb_path_view path_view(const char *text)
{
    tb_path_view view = {0};
    view.data = text;
    while (text[view.length] != '\0')
    {
        view.length++;
    }
    return view;
}
static void child_name(char *output, size_t capacity, const char *parent, const char *child)
{
    size_t left = {0};
    size_t right = {0};
    left = path_view(parent).length;
    right = path_view(child).length;
    TB_TEST_REQUIRE(left + right + 2 <= capacity);
    memcpy(output, parent, left);
    output[left] = '/';
    memcpy(output + left + 1, child, right + 1);
}

TB_TEST(platform_files, binary_roundtrips_cursor_sizes_and_eof)
{
    static const size_t lengths[] = {0, 1, 4095, 4096, 4097, 8192};
    char directory[32768] = {0};
    char path[32768] = {0};
    unsigned char source[8192] = {0};
    unsigned char output[8192] = {0};
    tb_file file = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, directory, sizeof(directory)), ==,
                         TB_FS_OK);
    child_name(path, sizeof(path), directory, "space \xE6\x97\xA5\xF0\x9F\x98\x80.bin");
    tb_test_random_bytes(source, sizeof(source));
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        size_t used = {0};
        size_t count = {0};
        uint64_t position = {0};
        uint64_t size = {0};
        tb_fs_info info = {0};
        TB_TEST_REQUIRE_UINT(
            tb_file_open(path_view(path),
                         TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_TRUNCATE, &file),
            ==, TB_FS_OK);
        while (used < lengths[row])
        {
            size_t chunk = {0};
            chunk = 1 + (size_t)tb_test_random_below(lengths[row] - used);
            TB_TEST_REQUIRE_UINT(tb_file_write(&file, source + used, chunk, &count), ==, TB_FS_OK);
            TB_TEST_REQUIRE(count > 0 && count <= chunk);
            used += count;
        }
        TB_TEST_EXPECT_UINT(tb_file_write(&file, NULL, 0, &count), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(count, ==, 0);
        TB_TEST_EXPECT_UINT(tb_file_tell(&file, &position), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(position, ==, lengths[row]);
        TB_TEST_EXPECT_UINT(tb_file_size(&file, &size), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(size, ==, lengths[row]);
        TB_TEST_EXPECT_UINT(tb_file_flush(&file), ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(file.value, ==, 0);
        TB_TEST_EXPECT_UINT(tb_file_close(&file), ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_fs_query(path_view(path), &info), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(info.kind, ==, TB_FS_REGULAR);
        TB_TEST_EXPECT_UINT(info.size, ==, lengths[row]);
        TB_TEST_EXPECT(info.modified_seconds > 0);
        TB_TEST_EXPECT(info.modified_nanoseconds < 1000000000);
        TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path), TB_FILE_READ, &file), ==, TB_FS_OK);
        used = 0;
        while (used < lengths[row])
        {
            size_t chunk = {0};
            chunk = 1 + (size_t)tb_test_random_below(lengths[row] - used);
            TB_TEST_REQUIRE_UINT(tb_file_read(&file, output + used, chunk, &count), ==, TB_FS_OK);
            TB_TEST_REQUIRE(count > 0 && count <= chunk);
            used += count;
        }
        TB_TEST_EXPECT(memcmp(source, output, lengths[row]) == 0);
        TB_TEST_EXPECT_UINT(tb_file_read(&file, output, sizeof(output), &count), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(count, ==, 0);
        TB_TEST_EXPECT_UINT(tb_file_read(&file, NULL, 0, &count), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(tb_file_seek(&file, INT64_C(4294967303), TB_FILE_START, &position), ==,
                            TB_FS_OK);
        TB_TEST_EXPECT_UINT(position, ==, UINT64_C(4294967303));
        TB_TEST_EXPECT_UINT(tb_file_read(&file, output, sizeof(output), &count), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(count, ==, 0);
        TB_TEST_EXPECT_UINT(tb_file_seek(&file, 0, TB_FILE_END, &position), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(position, ==, lengths[row]);
        TB_TEST_EXPECT_UINT(tb_file_seek(&file, -(int64_t)lengths[row], TB_FILE_CURRENT, &position),
                            ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(position, ==, 0);
        TB_TEST_EXPECT_UINT(tb_file_seek(&file, -1, TB_FILE_START, &position), ==, TB_FS_INVALID);
        TB_TEST_EXPECT_UINT(tb_file_seek(&file, 0, (tb_file_origin)999, &position), ==,
                            TB_FS_INVALID);
        TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(directory)), ==, TB_FS_OK);
}

TB_TEST(platform_files, errors_rename_and_long_paths)
{
    char root[32768] = {0};
    char path[32768] = {0};
    char renamed[32768] = {0};
    char nested[32768] = {0};
    size_t marks[6] = {0};
    tb_file file = {0};
    tb_fs_info info = {0};
    tb_directory iterator = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    child_name(path, sizeof(path), root, "one");
    child_name(renamed, sizeof(renamed), root, "two");
    TB_TEST_EXPECT_UINT(tb_file_open(path_view(path), TB_FILE_READ, &file), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_query(path_view(path), &info), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_file_open(path_view(root), TB_FILE_READ, &file), ==, TB_FS_IS_DIRECTORY);
    TB_TEST_EXPECT_UINT(tb_fs_make_directory(path_view(root)), ==, TB_FS_EXISTS);
    TB_TEST_REQUIRE_UINT(
        tb_file_open(path_view(path), TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE, &file),
        ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_NOT_EMPTY);
    TB_TEST_EXPECT_UINT(tb_directory_open(path_view(path), &iterator), ==, TB_FS_NOT_DIRECTORY);
    TB_TEST_EXPECT_UINT(
        tb_file_open(path_view(path), TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE, &file),
        ==, TB_FS_EXISTS);
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(renamed), TB_FILE_WRITE | TB_FILE_CREATE, &file),
                         ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_rename(path_view(path), path_view(renamed)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_query(path_view(path), &info), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(renamed)), ==, TB_FS_OK);
    memcpy(nested, root, path_view(root).length + 1);
    for (size_t level = {0}; level < TB_TEST_COUNT_OF(marks); level++)
    {
        marks[level] = path_view(nested).length;
        child_name(path, sizeof(path), nested,
                   "abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz");
        TB_TEST_REQUIRE_UINT(tb_fs_make_directory(path_view(path)), ==, TB_FS_OK);
        memcpy(nested, path, path_view(path).length + 1);
    }
    TB_TEST_REQUIRE(path_view(nested).length > 260);
    TB_TEST_REQUIRE_UINT(tb_fs_temp_file(path_view(nested), path, sizeof(path), &file), ==,
                         TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    for (size_t level = {0}; level < TB_TEST_COUNT_OF(marks); level++)
    {
        TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(nested)), ==, TB_FS_OK);
        nested[marks[TB_TEST_COUNT_OF(marks) - level - 1]] = '\0';
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_OK);
}

TB_TEST(platform_files, enumeration_retains_short_entries_and_visits_every_name_once)
{
    char root[32768] = {0};
    char path[32768] = {0};
    char name[64] = {0};
    unsigned char seen[10000] = {0};
    tb_directory iterator = {0};
    size_t required = {0};
    size_t count = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_directory_open(path_view(root), &iterator), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_directory_next(&iterator, name, sizeof(name), &required), ==, TB_FS_END);
    TB_TEST_EXPECT_UINT(required, ==, 0);
    TB_TEST_EXPECT_UINT(tb_directory_close(&iterator), ==, TB_FS_OK);
    for (size_t index = {0}; index < sizeof(seen); index++)
    {
        tb_file file = {0};
        size_t value = {0};
        value = index;
        for (size_t digit = {0}; digit < 4; digit++)
        {
            name[3 - digit] = (char)('0' + value % 10);
            value /= 10;
        }
        name[4] = '\0';
        child_name(path, sizeof(path), root, name);
        TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path),
                                          TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE,
                                          &file),
                             ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_directory_open(path_view(root), &iterator), ==, TB_FS_OK);
    for (;;)
    {
        tb_fs_result result = {0};
        size_t value = {0};
        result = tb_directory_next(&iterator, NULL, 0, &required);
        if (result == TB_FS_END)
        {
            break;
        }
        TB_TEST_REQUIRE_UINT(result, ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(required, ==, 5);
        name[0] = 42;
        TB_TEST_REQUIRE_UINT(tb_directory_next(&iterator, name, 4, &required), ==, TB_FS_NO_SPACE);
        TB_TEST_REQUIRE_INT(name[0], ==, 42);
        TB_TEST_REQUIRE_UINT(tb_directory_next(&iterator, name, sizeof(name), &required), ==,
                             TB_FS_OK);
        for (size_t digit = {0}; digit < 4; digit++)
        {
            TB_TEST_REQUIRE(name[digit] >= '0' && name[digit] <= '9');
            value = value * 10 + (size_t)(name[digit] - '0');
        }
        TB_TEST_REQUIRE(value < sizeof(seen));
        TB_TEST_REQUIRE_UINT(seen[value], ==, 0);
        seen[value] = 1;
        count++;
        child_name(path, sizeof(path), root, name);
        /* Mutation is outside the iterator contract: delete only after close. */
    }
    TB_TEST_REQUIRE_UINT(tb_directory_close(&iterator), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_directory_close(&iterator), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, sizeof(seen));
    for (size_t index = {0}; index < sizeof(seen); index++)
    {
        size_t value = {0};
        value = index;
        TB_TEST_EXPECT_UINT(seen[index], ==, 1);
        for (size_t digit = {0}; digit < 4; digit++)
        {
            name[3 - digit] = (char)('0' + value % 10);
            value /= 10;
        }
        name[4] = '\0';
        child_name(path, sizeof(path), root, name);
        TB_TEST_REQUIRE_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_OK);
}

TB_TEST(platform_files, locations_validate_text_and_never_partially_write)
{
    char path[32768] = {0};
    size_t required = {0};
    tb_file file = {0};
    tb_fs_info info = {0};
    static const char *const invalid[] = {"",
                                          "a\xFF",
                                          "\xC0\xAF",
                                          "\xED\xA0\x80",
                                          "\xF4\x90\x80\x80",
                                          "\xE0\x80\x80",
                                          "\xF0\x80\x80\x80",
                                          "\xE2\x82",
                                          "\xE2x\xAC"};
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(invalid); index++)
    {
        TB_TEST_EXPECT_UINT(tb_file_open(path_view(invalid[index]), TB_FILE_READ, &file), ==,
                            TB_FS_INVALID);
        TB_TEST_EXPECT_UINT(tb_fs_query(path_view(invalid[index]), &info), ==, TB_FS_INVALID);
    }
    TB_TEST_EXPECT_UINT(tb_fs_query((tb_path_view){"a\0b", 3}, &info), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_file_open(path_view("x"), 0, &file), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_file_open(path_view("x"), TB_FILE_READ | TB_FILE_TRUNCATE, &file), ==,
                        TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_file_open(path_view("x"), TB_FILE_READ | TB_FILE_EXCLUSIVE, &file), ==,
                        TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(tb_file_open(path_view("x"), 128, &file), ==, TB_FS_INVALID);
    for (size_t value = {0}; value <= TB_FS_TEMP; value++)
    {
        TB_TEST_REQUIRE_UINT(tb_fs_get_location((tb_fs_location)value, NULL, 0, &required), ==,
                             TB_FS_OK);
        TB_TEST_REQUIRE(required > 1 && required <= sizeof(path));
        path[0] = 42;
        TB_TEST_EXPECT_UINT(
            tb_fs_get_location((tb_fs_location)value, path, required - 1, &required), ==,
            TB_FS_NO_SPACE);
        TB_TEST_EXPECT_INT(path[0], ==, 42);
        TB_TEST_REQUIRE_UINT(
            tb_fs_get_location((tb_fs_location)value, path, sizeof(path), &required), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(path_view(path).length + 1, ==, required);
    }
    TB_TEST_EXPECT_UINT(tb_fs_get_location((tb_fs_location)999, path, sizeof(path), &required), ==,
                        TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(required, ==, 0);
    path[0] = 42;
    TB_TEST_EXPECT_UINT(tb_fs_temp_file((tb_path_view){0}, path, 1, &file), ==, TB_FS_NO_SPACE);
    TB_TEST_EXPECT_INT(path[0], ==, 42);
    TB_TEST_EXPECT_UINT(file.value, ==, 0);
}
