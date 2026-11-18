#include "badger/assert.h"
#include "badger/file.h"
#include "badger/thread.h"
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

/* Positional calls name their own offset, so the cursor plays no part: they
 * read and write anywhere, extend the file, and find the end without an error. */
TB_TEST(platform_files, positional_transfers_ignore_the_cursor_and_extend_the_file)
{
    static const unsigned char zeros[10] = {0};
    char directory[32768] = {0};
    char path[32768] = {0};
    unsigned char output[32] = {0};
    tb_file file = {0};
    size_t count = {0};
    uint64_t position = {0};
    uint64_t size = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, directory, sizeof(directory)), ==,
                         TB_FS_OK);
    child_name(path, sizeof(path), directory, "positional.bin");
    TB_TEST_REQUIRE_UINT(
        tb_file_open(path_view(path), TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE, &file), ==,
        TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_write_at(&file, 0, "0123456789", 10, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 10);
    TB_TEST_REQUIRE_UINT(tb_file_read_at(&file, 3, output, 4, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 4);
    TB_TEST_EXPECT(memcmp(output, "3456", 4) == 0);
    TB_TEST_REQUIRE_UINT(tb_file_read_at(&file, 7, output, sizeof(output), &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 3);
    TB_TEST_EXPECT(memcmp(output, "789", 3) == 0);
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, 10, output, sizeof(output), &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, 4096, output, sizeof(output), &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, 0, NULL, 0, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);

    /* Writing past the end extends the file, and the gap reads as zero. */
    TB_TEST_REQUIRE_UINT(tb_file_write_at(&file, 20, "ab", 2, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    TB_TEST_EXPECT_UINT(tb_file_size(&file, &size), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(size, ==, 22);
    TB_TEST_REQUIRE_UINT(tb_file_read_at(&file, 10, output, 12, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 12);
    TB_TEST_EXPECT(memcmp(output, zeros, 10) == 0);
    TB_TEST_EXPECT(memcmp(output + 10, "ab", 2) == 0);

    /* The cursor is unspecified afterwards; a seek makes it usable again. */
    TB_TEST_EXPECT_UINT(tb_file_seek(&file, 1, TB_FILE_START, &position), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_read(&file, output, 2, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    TB_TEST_EXPECT(memcmp(output, "12", 2) == 0);

    /* Offsets beyond int64_t are malformed. A read there finds the end; a
     * write that cannot fit a byte before INT64_MAX is too large. */
    count = 99;
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, (uint64_t)INT64_MAX + 1, output, 1, &count), ==,
                        TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    count = 99;
    TB_TEST_EXPECT_UINT(tb_file_write_at(&file, UINT64_MAX, "x", 1, &count), ==, TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, (uint64_t)INT64_MAX, output, 1, &count), ==,
                        TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_file_write_at(&file, (uint64_t)INT64_MAX, "x", 1, &count), ==,
                        TB_FS_TOO_LARGE);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_file_write_at(&file, (uint64_t)INT64_MAX, NULL, 0, &count), ==,
                        TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_size(&file, &size), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(size, ==, 22);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);

    /* Access is checked as for the cursor functions. */
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path), TB_FILE_READ, &file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_write_at(&file, 0, "x", 1, &count), ==, TB_FS_ACCESS);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path), TB_FILE_WRITE, &file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_read_at(&file, 0, output, 1, &count), ==, TB_FS_ACCESS);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(directory)), ==, TB_FS_OK);
}

enum
{
    POSITIONAL_THREADS = 8,
    POSITIONAL_BLOCK = 4096,
    POSITIONAL_BLOCKS_PER_THREAD = 16,
    POSITIONAL_FILE_SIZE = POSITIONAL_THREADS * POSITIONAL_BLOCKS_PER_THREAD * POSITIONAL_BLOCK,
    POSITIONAL_READS = 2000
};

/* The byte every test file holds at an offset, so a reader can check any range
 * without a copy of the file. Odd multipliers make neighbouring blocks differ. */
static unsigned char positional_byte(uint64_t offset)
{
    return (unsigned char)((offset * UINT64_C(2654435761)) >> 13);
}

typedef struct positional_worker
{
    tb_file *file;
    uint64_t seed;
    size_t index;
    size_t mismatches;
    size_t failures;
} positional_worker;

/* Each thread has a generator of its own: the test framework's is not shared
 * between threads. */
static uint64_t positional_next(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void positional_write_blocks(void *argument)
{
    positional_worker *const worker = argument;
    unsigned char block[POSITIONAL_BLOCK] = {0};
    for (size_t step = {0}; step < POSITIONAL_BLOCKS_PER_THREAD; step++)
    {
        /* Threads interleave their blocks, so neighbours are written at once. */
        const uint64_t number = (uint64_t)step * POSITIONAL_THREADS + worker->index;
        const uint64_t offset = number * POSITIONAL_BLOCK;
        size_t done = {0};
        for (size_t byte = {0}; byte < POSITIONAL_BLOCK; byte++)
        {
            block[byte] = positional_byte(offset + byte);
        }
        while (done < POSITIONAL_BLOCK)
        {
            size_t count = {0};
            if (tb_file_write_at(worker->file, offset + done, block + done, POSITIONAL_BLOCK - done,
                                 &count) != TB_FS_OK ||
                count == 0)
            {
                worker->failures++;
                break;
            }
            done += count;
        }
    }
}

static void positional_read_ranges(void *argument)
{
    positional_worker *const worker = argument;
    unsigned char buffer[POSITIONAL_BLOCK] = {0};
    uint64_t state = worker->seed;
    for (size_t read = {0}; read < POSITIONAL_READS; read++)
    {
        /* Ranges of any length, overlapping each other and the end of the file. */
        const uint64_t offset = positional_next(&state) % (POSITIONAL_FILE_SIZE + 64);
        const size_t capacity = 1 + (size_t)(positional_next(&state) % POSITIONAL_BLOCK);
        const uint64_t available =
            offset < POSITIONAL_FILE_SIZE ? POSITIONAL_FILE_SIZE - offset : 0;
        size_t count = {0};
        if (tb_file_read_at(worker->file, offset, buffer, capacity, &count) != TB_FS_OK ||
            count > capacity || (uint64_t)count > available || (count == 0 && available > 0))
        {
            worker->failures++;
            continue;
        }
        for (size_t byte = {0}; byte < count; byte++)
        {
            if (buffer[byte] != positional_byte(offset + byte))
            {
                worker->mismatches++;
                break;
            }
        }
    }
}

static void positional_run(tb_thread_function *entry, tb_file *file,
                           positional_worker workers[POSITIONAL_THREADS])
{
    tb_thread threads[POSITIONAL_THREADS] = {0};
    for (size_t index = {0}; index < POSITIONAL_THREADS; index++)
    {
        workers[index] = (positional_worker){0};
        workers[index].file = file;
        workers[index].index = index;
        workers[index].seed = UINT64_C(0x9E3779B97F4A7C15) * (index + 1);
        tb_thread_create(&threads[index], entry, &workers[index]);
    }
    for (size_t index = {0}; index < POSITIONAL_THREADS; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

/* The claim of file.h: positional calls run together on one handle. Writers
 * fill disjoint blocks at once, then readers check overlapping ranges at once. */
TB_TEST(platform_files, positional_transfers_run_together_on_one_handle)
{
    char directory[32768] = {0};
    char path[32768] = {0};
    positional_worker workers[POSITIONAL_THREADS] = {0};
    tb_file file = {0};
    uint64_t size = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, directory, sizeof(directory)), ==,
                         TB_FS_OK);
    child_name(path, sizeof(path), directory, "shared.bin");
    TB_TEST_REQUIRE_UINT(
        tb_file_open(path_view(path), TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE, &file), ==,
        TB_FS_OK);
    positional_run(positional_write_blocks, &file, workers);
    for (size_t index = {0}; index < POSITIONAL_THREADS; index++)
    {
        TB_TEST_EXPECT_UINT(workers[index].failures, ==, 0);
    }
    TB_TEST_EXPECT_UINT(tb_file_size(&file, &size), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(size, ==, POSITIONAL_FILE_SIZE);
    positional_run(positional_read_ranges, &file, workers);
    for (size_t index = {0}; index < POSITIONAL_THREADS; index++)
    {
        TB_TEST_EXPECT_UINT(workers[index].failures, ==, 0);
        TB_TEST_EXPECT_UINT(workers[index].mismatches, ==, 0);
    }
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
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
        TB_TEST_REQUIRE_UINT(tb_directory_next(&iterator, name, 4, &required), ==,
                             TB_FS_BUFFER_TOO_SMALL);
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
            TB_FS_BUFFER_TOO_SMALL);
        TB_TEST_EXPECT_INT(path[0], ==, 42);
        TB_TEST_REQUIRE_UINT(
            tb_fs_get_location((tb_fs_location)value, path, sizeof(path), &required), ==, TB_FS_OK);
        TB_TEST_EXPECT_UINT(path_view(path).length + 1, ==, required);
    }
    TB_TEST_EXPECT_UINT(tb_fs_get_location((tb_fs_location)999, path, sizeof(path), &required), ==,
                        TB_FS_INVALID);
    TB_TEST_EXPECT_UINT(required, ==, 0);
    path[0] = 42;
    TB_TEST_EXPECT_UINT(tb_fs_temp_file((tb_path_view){0}, path, 1, &file), ==,
                        TB_FS_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_INT(path[0], ==, 42);
    TB_TEST_EXPECT_UINT(file.value, ==, 0);
}

TB_TEST(platform_files, failures_preserve_outputs_and_case_behavior_is_native)
{
    char root[32768] = {0};
    char path[32768] = {0};
    char other[32768] = {0};
    char nested[32768] = {0};
    tb_file file = {0};
    tb_directory directory = {0};
    size_t transferred = {0};
    char byte = {0};
    tb_fs_info info = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    child_name(path, sizeof(path), root, "missing/child");
    child_name(other, sizeof(other), root, "another/child");
    TB_TEST_EXPECT_UINT(tb_file_open(path_view(path), TB_FILE_WRITE | TB_FILE_CREATE, &file), ==,
                        TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_make_directory(path_view(path)), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_directory_open(path_view(path), &directory), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_rename(path_view(path), path_view(other)), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_temp_directory(path_view(path), nested, sizeof(nested)), ==,
                        TB_FS_NOT_FOUND);
    info.size = 123;
    TB_TEST_EXPECT_UINT(tb_fs_query(path_view(path), &info), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(info.size, ==, 123);
    child_name(path, sizeof(path), root, "COM10");
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path), TB_FILE_WRITE | TB_FILE_CREATE, &file), ==,
                         TB_FS_OK);
    TB_TEST_EXPECT(tb_file_read(&file, &byte, 1, &transferred) != TB_FS_OK);
    TB_TEST_EXPECT_UINT(transferred, ==, 0);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(path), TB_FILE_READ, &file), ==, TB_FS_OK);
    TB_TEST_EXPECT(tb_file_write(&file, &byte, 1, &transferred) != TB_FS_OK);
    TB_TEST_EXPECT_UINT(transferred, ==, 0);
    TB_TEST_REQUIRE_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_rename(path_view(path), path_view(other)), ==, TB_FS_NOT_FOUND);
    child_name(nested, sizeof(nested), path, "child");
    {
        tb_fs_result result = {0};
        result = tb_fs_query(path_view(nested), &info);
        TB_TEST_EXPECT(result == TB_FS_NOT_DIRECTORY || result == TB_FS_NOT_FOUND);
    }
    child_name(other, sizeof(other), root, "com10");
    {
        tb_fs_result result = {0};
        result = tb_file_open(path_view(other), TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE,
                              &file);
        TB_TEST_REQUIRE(result == TB_FS_OK || result == TB_FS_EXISTS);
        if (result == TB_FS_OK)
        {
            TB_TEST_EXPECT_UINT(tb_file_close(&file), ==, TB_FS_OK);
            TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(other)), ==, TB_FS_OK);
        }
    }
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_OK);
}

TB_TEST(platform_files, windows_reserved_names_and_extended_paths)
{
    static const char *const reserved[] = {"NUL",   "con.txt",  "PRN",     "a/AUX.log",
                                           "COM1",  "LPT9.txt", "foo:bar", "name.",
                                           "name ", "a?b",      "a*b",     "//./disk"};
    char root[32768] = {0};
    char extended[32768] = {0};
    char file_path[32768] = {0};
    tb_fs_info info = {0};
    tb_file file = {0};
    if (tb_fs_native_style() != TB_PATH_WINDOWS)
    {
        TB_TEST_SKIP("Windows namespace rules");
    }
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(reserved); index++)
    {
        TB_TEST_EXPECT_UINT(tb_fs_query(path_view(reserved[index]), &info), ==, TB_FS_INVALID);
    }
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_INT(root[1], ==, ':');
    memcpy(extended, "//?/", 4);
    memcpy(extended + 4, root, path_view(root).length + 1);
    TB_TEST_EXPECT_UINT(tb_fs_query(path_view(extended), &info), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_fs_temp_file(path_view(extended), file_path, sizeof(file_path), &file),
                         ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_close(&file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(file_path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_OK);
}

static void invalid_file_contract(void *context)
{
    size_t choice = {0};
    size_t count = {0};
    uint64_t position = {0};
    tb_file file = {0};
    tb_directory directory = {0};
    char byte = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        (void)tb_file_open(path_view("x"), TB_FILE_READ, NULL);
        break;
    case 1:
        (void)tb_file_open((tb_path_view){NULL, 1}, TB_FILE_READ, &file);
        break;
    case 2:
        (void)tb_file_read(&file, &byte, 1, &count);
        break;
    case 3:
        (void)tb_file_read(&file, &byte, 1, NULL);
        break;
    case 4:
        (void)tb_file_read(&file, NULL, 1, &count);
        break;
    case 5:
        (void)tb_file_write(&file, &byte, 1, &count);
        break;
    case 6:
        (void)tb_file_write(&file, NULL, 1, &count);
        break;
    case 7:
        (void)tb_file_write(&file, &byte, 1, NULL);
        break;
    case 8:
        (void)tb_file_seek(&file, 0, TB_FILE_START, &position);
        break;
    case 9:
        (void)tb_file_seek(&file, 0, TB_FILE_START, NULL);
        break;
    case 10:
        (void)tb_file_size(&file, NULL);
        break;
    case 11:
        (void)tb_file_size(&file, &position);
        break;
    case 12:
        (void)tb_file_flush(&file);
        break;
    case 13:
        (void)tb_file_close(NULL);
        break;
    case 14:
        (void)tb_fs_query(path_view("x"), NULL);
        break;
    case 15:
        (void)tb_directory_open(path_view("x"), NULL);
        break;
    case 16:
        (void)tb_directory_next(&directory, &byte, 1, &count);
        break;
    case 17:
        (void)tb_directory_close(NULL);
        break;
    case 18:
        (void)tb_fs_get_location(TB_FS_TEMP, NULL, 1, &count);
        break;
    case 19:
        (void)tb_fs_get_location(TB_FS_TEMP, &byte, 1, NULL);
        break;
    case 20:
        (void)tb_fs_temp_file((tb_path_view){0}, &byte, 1, NULL);
        break;
    case 21:
        (void)tb_fs_temp_directory((tb_path_view){0}, NULL, 0);
        break;
    case 22:
        (void)tb_file_read_at(&file, 0, &byte, 1, &count);
        break;
    case 23:
        (void)tb_file_read_at(&file, 0, &byte, 1, NULL);
        break;
    case 24:
        (void)tb_file_read_at(&file, 0, NULL, 1, &count);
        break;
    case 25:
        (void)tb_file_write_at(&file, 0, &byte, 1, &count);
        break;
    case 26:
        (void)tb_file_write_at(&file, 0, &byte, 1, NULL);
        break;
    case 27:
        (void)tb_file_write_at(&file, 0, NULL, 1, &count);
        break;
    default:
        break;
    }
}
TB_TEST(platform_files, required_pointers_and_handle_lifetimes_are_contracts)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 28; choice++)
    {
        TB_TEST_EXPECT_TRAP(invalid_file_contract, &choice, "assertion failed");
    }
}

TB_TEST(platform_files, replacement_keeps_open_readers_on_the_previous_file)
{
    char root[32768] = {0};
    char old_path[32768] = {0};
    char new_path[32768] = {0};
    tb_file old_file = {0};
    tb_file new_file = {0};
    size_t count = {0};
    char byte = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_fs_temp_file(path_view(root), old_path, sizeof(old_path), &old_file),
                         ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_fs_temp_file(path_view(root), new_path, sizeof(new_path), &new_file),
                         ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_write(&old_file, "o", 1, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_write(&new_file, "n", 1, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_close(&old_file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_close(&new_file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(old_path), TB_FILE_READ, &old_file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_fs_rename(path_view(new_path), path_view(old_path)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_read(&old_file, &byte, 1, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 1);
    TB_TEST_EXPECT_INT(byte, ==, 'o');
    TB_TEST_REQUIRE_UINT(tb_file_open(path_view(old_path), TB_FILE_READ, &new_file), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_file_read(&new_file, &byte, 1, &count), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(count, ==, 1);
    TB_TEST_EXPECT_INT(byte, ==, 'n');
    TB_TEST_EXPECT_UINT(tb_file_close(&old_file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_file_close(&new_file), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(old_path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(root)), ==, TB_FS_OK);
}
