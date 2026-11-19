/* The headers call some functions thread-safe and some objects independent of
 * one another. These cases take them at their word: eight threads use them at
 * once, each with data of its own, and every thread must get the answers that
 * one thread gets alone.
 *
 * On one thread the cases prove little. They are for ThreadSanitizer, which
 * reports any memory that two threads touch without a lock: state that a
 * function keeps and does not mention, or something shared between two
 * objects that are documented as independent. Modules that synchronize
 * themselves (the logger, a shared table of atoms, the job system, the
 * checking memory functions) have cases of their own beside their other
 * tests. A thread started by a case only records; the case checks after
 * joining. */
#include "badger/ap.h"
#include "badger/arena.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/hash.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "badger/utf8.h"
#include "badger/xp.h"
#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define THREAD_COUNT 8
#define PATH_CAPACITY ((size_t)32768)

typedef struct worker
{
    size_t number;      /* Which of the threads this is, from 0. */
    const char *root;   /* A directory that the threads share, when they need one. */
    size_t wrong_count; /* Answers that were not what one thread gets alone. */
} worker;

static void run_on_every_thread(tb_thread_function *function, worker *workers, const char *root)
{
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        workers[index].number = index;
        workers[index].root = root;
        workers[index].wrong_count = 0;
        tb_thread_create(&threads[index], function, &workers[index]);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

static void expect_no_wrong_answer(const worker *workers)
{
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_CONTEXT("thread %zu", index);
        TB_TEST_EXPECT_UINT(workers[index].wrong_count, ==, 0);
    }
}

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

static void count_unless(worker *self, bool is_right)
{
    if (!is_right)
    {
        self->wrong_count++;
    }
}

/* --- functions that keep no state ---------------------------------------------- */

#define STATELESS_ROUNDS ((size_t)2000)

/* Extended-precision arithmetic on a number made from the thread's number:
 * (number + 1) * 10^30, written as text, read back, and divided again. */
static bool uses_extended_precision(size_t number)
{
    tb_xp_digit value[4] = {0};
    tb_xp_digit power[4] = {0};
    tb_xp_digit product[8] = {0};
    tb_xp_digit parsed[8] = {0};
    tb_xp_digit quotient[8] = {0};
    tb_xp_digit remainder[4] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(8, 4)] = {0};
    char expected[48] = {0};
    char text[48] = {0};
    size_t required = 0;
    uint64_t back = 0;
    bool is_right = true;

    (void)tb_xp_from_u64(4, value, number + 1);
    (void)tb_xp_from_string(4, power, "1000000000000000000000000000000", 31, 10);
    tb_xp_mul(product, 4, value, 4, power);
    (void)tb_fmt_buffer(expected, sizeof(expected), "%zu000000000000000000000000000000",
                        number + 1);
    is_right = is_right &&
               tb_xp_to_string(8, product, 10, scratch, text, sizeof(text), &required) == TB_XP_OK;
    is_right = is_right && tb_str_equal(text, expected);
    is_right = is_right && tb_xp_from_string(8, parsed, text, required - 1, 10) == TB_XP_OK;
    tb_xp_div(8, quotient, parsed, 4, power, remainder, scratch);
    is_right = is_right && tb_xp_to_u64(8, quotient, &back) && back == number + 1;
    return is_right;
}

/* Arbitrary-precision values on the engine's heap: -(number + 1)^40, its text,
 * and the value read back and divided by (number + 1)^39. */
static bool uses_arbitrary_precision(size_t number)
{
    tb_ap *const base = tb_ap_from_i64(NULL, -(int64_t)number - 1);
    tb_ap *const exponent = tb_ap_from_i64(NULL, 40);
    tb_ap *const smaller_exponent = tb_ap_from_i64(NULL, 39);
    tb_ap *const power = tb_ap_pow(base, exponent, NULL);
    tb_ap *const divisor = tb_ap_pow(base, smaller_exponent, NULL);
    tb_ap *read_back = NULL;
    tb_ap *quotient = NULL;
    char text[128] = {0};
    size_t required = 0;
    int64_t value = 0;
    bool is_right = tb_ap_to_string(power, 10, text, sizeof(text), &required) == TB_AP_OK;

    is_right = is_right && tb_ap_from_string(NULL, text, required - 1, 10, &read_back) == TB_AP_OK;
    if (is_right)
    {
        quotient = tb_ap_div_floor(read_back, divisor);
        is_right = tb_ap_to_i64(quotient, &value) && value == -(int64_t)number - 1;
    }
    tb_ap_free(quotient);
    tb_ap_free(read_back);
    tb_ap_free(divisor);
    tb_ap_free(power);
    tb_ap_free(smaller_exponent);
    tb_ap_free(exponent);
    tb_ap_free(base);
    return is_right;
}

/* A double made from the thread's number, written as its shortest text and
 * with %.17g and %a, and read back from each. */
static bool converts_floating_point(size_t number)
{
    const double value = ((double)number + 1.0) / 3.0;
    char shortest[TB_DECIMAL_SHORTEST_SIZE] = {0};
    char digits[48] = {0};
    char hexadecimal[48] = {0};
    size_t required = 0;
    double back = 0.0;
    bool is_right =
        tb_decimal_shortest(value, shortest, sizeof(shortest), &required) == TB_DECIMAL_OK;

    is_right = is_right && tb_decimal_parse(shortest, required - 1, &back) == TB_DECIMAL_OK &&
               back == value;
    is_right = is_right &&
               tb_decimal_parse(digits, tb_fmt_buffer(digits, sizeof(digits), "%.17g", value),
                                &back) == TB_DECIMAL_OK &&
               back == value;
    is_right =
        is_right &&
        tb_decimal_parse(hexadecimal, tb_fmt_buffer(hexadecimal, sizeof(hexadecimal), "%a", value),
                         &back) == TB_DECIMAL_OK &&
        back == value;
    return is_right;
}

/* Paths, UTF-8, hashing, C strings, formatting to a buffer, extended- and
 * arbitrary-precision arithmetic, and floating-point text, each on data that carries the thread's
 * number, so that an answer leaking from one thread to another would be a
 * wrong answer. */
static void use_the_functions_that_keep_no_state(void *argument)
{
    worker *const self = argument;
    char tangled[64] = {0};
    char expected[64] = {0};
    const size_t tangled_length = tb_fmt_buffer(
        tangled, sizeof(tangled), "assets/./t%zu/../t%zu//mesh.bin/", self->number, self->number);
    const size_t expected_length =
        tb_fmt_buffer(expected, sizeof(expected), "assets/t%zu/mesh.bin", self->number);
    const uint64_t expected_hash = tb_hash_bytes(expected, expected_length);

    for (size_t round = 0; round < STATELESS_ROUNDS; round++)
    {
        char normalized[64] = {0};
        char joined[96] = {0};
        char back[64] = {0};
        uint16_t wide[64] = {0};
        size_t required = 0;
        size_t units = 0;
        size_t bytes = 0;

        count_unless(self, tb_path_normalize(TB_PATH_POSIX, view(tangled), normalized,
                                             sizeof(normalized), &required) == TB_PATH_OK);
        count_unless(self, required == expected_length + 1 && tb_str_equal(normalized, expected));
        count_unless(self, tb_path_join(TB_PATH_WINDOWS, view("C:\\game"), view(normalized), joined,
                                        sizeof(joined), &required) == TB_PATH_OK);
        count_unless(self, tb_str_ends_with(joined, expected));

        count_unless(self, tb_utf8_validate(normalized, expected_length, &required));
        count_unless(self, tb_utf8_to_utf16(normalized, expected_length, wide,
                                            TB_TEST_COUNT_OF(wide), &units));
        count_unless(self, tb_utf8_from_utf16(wide, units, back, sizeof(back), &bytes));
        count_unless(self,
                     bytes == expected_length && memcmp(back, expected, expected_length) == 0);

        count_unless(self, tb_hash_bytes(normalized, expected_length) == expected_hash);
        count_unless(self, tb_hash_string(normalized) == expected_hash);
        count_unless(self,
                     tb_str_find(normalized, 0, expected_length, "mesh") == expected_length - 8);
        count_unless(self, tangled_length > expected_length);

        count_unless(self, uses_extended_precision(self->number));
        count_unless(self, uses_arbitrary_precision(self->number));
        count_unless(self, converts_floating_point(self->number));
    }
}

TB_TEST(core_thread_safety, functions_that_keep_no_state_agree_on_every_thread)
{
    worker workers[THREAD_COUNT] = {0};

    run_on_every_thread(use_the_functions_that_keep_no_state, workers, NULL);
    expect_no_wrong_answer(workers);
}

/* --- arenas --------------------------------------------------------------------- */

#define ARENA_BLOCKS ((size_t)400)
#define ARENA_RESERVE ((size_t)8 << 20)

/* An arena of each kind, used by one thread. Arenas are independent of one
 * another: the reserved kind takes its address space from the system and
 * commits it as it grows, and the system is all that two of them share. */
static void fill_and_reset_arenas_of_its_own(void *argument)
{
    worker *const self = argument;
    tb_arena *const arenas[] = {tb_arena_create_reserved(ARENA_RESERVE),
                                tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE)};

    for (size_t kind = 0; kind < TB_TEST_COUNT_OF(arenas); kind++)
    {
        for (size_t round = 0; round < 3; round++)
        {
            unsigned char *blocks[ARENA_BLOCKS] = {0};

            for (size_t index = 0; index < ARENA_BLOCKS; index++)
            {
                blocks[index] = tb_arena_alloc(arenas[kind], 100 + index, 16);
                memset(blocks[index], (int)(self->number * 31 + index), 100 + index);
            }
            for (size_t index = 0; index < ARENA_BLOCKS; index++)
            {
                const unsigned char fill = (unsigned char)(self->number * 31 + index);

                count_unless(self, blocks[index][0] == fill && blocks[index][99 + index] == fill);
            }
            tb_arena_reset(arenas[kind]);
        }
        tb_arena_destroy(arenas[kind]);
    }
}

TB_TEST(core_thread_safety, every_thread_can_fill_arenas_of_its_own)
{
    worker workers[THREAD_COUNT] = {0};

    run_on_every_thread(fill_and_reset_arenas_of_its_own, workers, NULL);
    expect_no_wrong_answer(workers);
}

/* --- files ---------------------------------------------------------------------- */

#define FILE_ROUNDS ((size_t)12)
#define FILE_SIZE ((size_t)6000)

/* A file of its own for each thread, in a directory they share: written
 * whole, read back, replaced, and then read through a handle a piece at a
 * time. Handles are independent, and so are calls that name different paths. */
static void write_read_and_replace_a_file_of_its_own(void *argument)
{
    worker *const self = argument;
    static _Thread_local char path[PATH_CAPACITY] = {0};
    static _Thread_local unsigned char content[FILE_SIZE] = {0};
    char name[32] = {0};
    size_t required = 0;

    (void)tb_fmt_buffer(name, sizeof(name), "thread-%zu.bin", self->number);
    count_unless(self, tb_path_join(tb_fs_native_style(), view(self->root), view(name), path,
                                    sizeof(path), &required) == TB_PATH_OK);
    for (size_t round = 0; round < FILE_ROUNDS; round++)
    {
        const unsigned char fill = (unsigned char)(self->number * 16 + round);
        tb_fs_buffer whole = {0};
        tb_fs_info info = {0};
        tb_file file = {0};
        unsigned char piece[512] = {0};
        size_t transferred = 0;
        uint64_t position = 0;

        memset(content, fill, sizeof(content));
        if (round % 2 == 0)
        {
            count_unless(self, tb_fs_write_file(view(path), content, sizeof(content)) == TB_FS_OK);
        }
        else
        {
            count_unless(self,
                         tb_fs_replace_file(view(path), content, sizeof(content)) == TB_FS_OK);
        }
        count_unless(self, tb_fs_query(view(path), &info) == TB_FS_OK);
        count_unless(self, tb_fs_read_file(view(path), NULL, FILE_SIZE, &whole) == TB_FS_OK);
        count_unless(self, whole.length == FILE_SIZE && whole.data != NULL &&
                               whole.data[0] == fill && whole.data[FILE_SIZE - 1] == fill);
        tb_fs_buffer_free(&whole);

        count_unless(self, tb_file_open(view(path), TB_FILE_READ, &file) == TB_FS_OK);
        count_unless(self, tb_file_seek(&file, 5000, TB_FILE_START, &position) == TB_FS_OK);
        count_unless(self, tb_file_read(&file, piece, sizeof(piece), &transferred) == TB_FS_OK);
        count_unless(self, transferred == sizeof(piece) && piece[0] == fill && piece[511] == fill);
        count_unless(self, tb_file_close(&file) == TB_FS_OK);
    }
    count_unless(self, tb_fs_remove(view(path)) == TB_FS_OK);
}

TB_TEST(core_thread_safety, every_thread_can_use_files_of_its_own)
{
    static char root[PATH_CAPACITY] = {0};
    worker workers[THREAD_COUNT] = {0};

    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    run_on_every_thread(write_read_and_replace_a_file_of_its_own, workers, root);
    expect_no_wrong_answer(workers);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

#define VERSION_SIZE ((size_t)16384)
#define VERSION_ROUNDS ((size_t)40)
#define WRITER_COUNT ((size_t)4)

/* Whether a file's bytes are one whole version: the right size, and every
 * byte the letter of one writer. */
static bool is_one_whole_version(const tb_fs_buffer *whole)
{
    if (whole->length != VERSION_SIZE || whole->data[0] < 'A' ||
        whole->data[0] >= 'A' + (unsigned char)WRITER_COUNT)
    {
        return false;
    }
    for (size_t index = 1; index < VERSION_SIZE; index++)
    {
        if (whole->data[index] != whole->data[0])
        {
            return false;
        }
    }
    return true;
}

/* The first threads replace one file over and over, each with a version of
 * its own letter; the rest read it. tb_fs_replace_file() promises that a
 * reader sees the old file or the new one. A reader that finds two letters in
 * the file, or part of a version, has seen a file that was being written.
 *
 * A call may fail while another thread has the file, which Windows reports
 * as access denied, and that is no wrong answer; half a file is. */
static void replace_or_read_the_one_file(void *argument)
{
    worker *const self = argument;
    static _Thread_local char path[PATH_CAPACITY] = {0};
    static _Thread_local unsigned char version[VERSION_SIZE] = {0};
    size_t required = 0;

    count_unless(self, tb_path_join(tb_fs_native_style(), view(self->root), view("shared.bin"),
                                    path, sizeof(path), &required) == TB_PATH_OK);
    memset(version, 'A' + (int)self->number, sizeof(version));
    for (size_t round = 0; round < VERSION_ROUNDS; round++)
    {
        tb_fs_buffer whole = {0};

        if (self->number < WRITER_COUNT)
        {
            (void)tb_fs_replace_file(view(path), version, sizeof(version));
            continue;
        }
        if (tb_fs_read_file(view(path), NULL, VERSION_SIZE * 2, &whole) == TB_FS_OK)
        {
            count_unless(self, is_one_whole_version(&whole));
            tb_fs_buffer_free(&whole);
        }
    }
}

TB_TEST(core_thread_safety, a_file_replaced_by_several_threads_is_always_one_whole_version)
{
    static char root[PATH_CAPACITY] = {0};
    static char path[PATH_CAPACITY] = {0};
    static unsigned char first[VERSION_SIZE] = {0};
    worker workers[THREAD_COUNT] = {0};
    tb_fs_buffer whole = {0};
    size_t required = 0;

    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("shared.bin"), path,
                                      sizeof(path), &required),
                         ==, TB_PATH_OK);
    memset(first, 'A', sizeof(first));
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), first, sizeof(first)), ==, TB_FS_OK);

    run_on_every_thread(replace_or_read_the_one_file, workers, root);
    expect_no_wrong_answer(workers);
    /* And what is left when they are done is one version too. */
    TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(path), NULL, VERSION_SIZE * 2, &whole), ==, TB_FS_OK);
    TB_TEST_EXPECT(is_one_whole_version(&whole));
    tb_fs_buffer_free(&whole);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
}

/* --- a JSON document, read by every thread at once ------------------------------ */

#define DOCUMENT_KEYS ((size_t)300)

static const tb_json_value *shared_document;

/* Every thread looks up every member of one document, and reads each value. */
static void read_the_one_document(void *argument)
{
    worker *const self = argument;
    for (size_t round = 0; round < 20; round++)
    {
        for (size_t index = 0; index < DOCUMENT_KEYS; index++)
        {
            const size_t key = (index + self->number * 37) % DOCUMENT_KEYS;
            char name[24] = {0};
            const size_t length = tb_fmt_buffer(name, sizeof(name), "member%zu", key);
            const tb_json_value *const value = tb_json_find(shared_document, name, length);
            int64_t number = 0;
            count_unless(self, value != NULL && tb_json_as_int64(value, &number) &&
                                   number == (int64_t)key);
        }
    }
}

TB_TEST(core_thread_safety, every_thread_can_read_one_json_document)
{
    worker workers[THREAD_COUNT] = {0};
    tb_arena *const arena = tb_arena_create(0);
    char *const text = tb_malloc(DOCUMENT_KEYS * 32 + 2);
    size_t length = 0;
    text[length++] = '{';
    for (size_t key = 0; key < DOCUMENT_KEYS; key++)
    {
        length +=
            tb_fmt_buffer(text + length, 32, "%s\"member%zu\":%zu", key == 0 ? "" : ",", key, key);
    }
    text[length++] = '}';
    TB_TEST_REQUIRE_UINT(tb_json_parse(text, length, NULL, arena, &shared_document, NULL), ==,
                         TB_JSON_OK);
    run_on_every_thread(read_the_one_document, workers, NULL);
    expect_no_wrong_answer(workers);
    shared_document = NULL;
    tb_free(text);
    tb_arena_destroy(arena);
}
