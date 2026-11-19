/* Packages: what is written reads back, plain and obfuscated, from many
 * threads at once; what obfuscation hides stays hidden; a wrong key is told
 * from a damaged package; and damaged, inconsistent, and hostile packages
 * are refused or reported, never trusted. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/fmt.h"
#include "badger/package.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stdbool.h>
#include <string.h>

static const uint8_t game_key[TB_PACKAGE_KEY_SIZE] = {
    0x9f, 0x2c, 0x41, 0xd0, 0x7e, 0x3a, 0xb5, 0x6c, 0x8d, 0x1e, 0x0f, 0x4a, 0x7b, 0x29, 0xc6, 0xe3,
    0x5d, 0x8a, 0x10, 0xf7, 0xb4, 0xe2, 0xc9, 0xd6, 0xa3, 0xf0, 0x58, 0x17, 0xe6, 0xc2, 0xb4, 0x9d};

#define TEXT_SIZE ((size_t)20000)
#define NOISE_SIZE ((size_t)3000)

/* A sentence no package should show in plain sight when it is obfuscated. */
static const char secret_line[] = "the dragon sleeps beneath the eastern tower\n";

typedef struct sample
{
    const char *name;
    const unsigned char *bytes;
    size_t size;
    tb_package_codec codec; /* What the writer should choose at level 6. */
} sample;

static unsigned char text[TEXT_SIZE];
static unsigned char noise[NOISE_SIZE];
static char long_name[700];

static void make_samples(sample *samples, size_t *count)
{
    uint64_t state = 12345;
    const size_t line = sizeof(secret_line) - 1;
    for (size_t index = 0; index < TEXT_SIZE; index++)
    {
        text[index] = (unsigned char)secret_line[index % line];
    }
    for (size_t index = 0; index < NOISE_SIZE; index++)
    {
        state = state * UINT64_C(6364136223846793005) + 1442695040888963407u;
        noise[index] = (unsigned char)(state >> 56);
    }
    for (size_t index = 0; index + 1 < sizeof(long_name); index++)
    {
        long_name[index] = (index % 50 == 49) ? '/' : 'x';
    }
    long_name[sizeof(long_name) - 2] = 'z';
    long_name[sizeof(long_name) - 1] = '\0';
    samples[0] = (sample){"textures/rock.txt", text, TEXT_SIZE, TB_PACKAGE_DEFLATED};
    samples[1] = (sample){"sounds/noise.bin", noise, NOISE_SIZE, TB_PACKAGE_STORED};
    samples[2] = (sample){"empty", NULL, 0, TB_PACKAGE_STORED};
    samples[3] = (sample){"a", text, 1, TB_PACKAGE_STORED};
    samples[4] = (sample){long_name, text, 100, TB_PACKAGE_DEFLATED};
    samples[5] = (sample){"\xc3\xbc"
                          "n"
                          "\xc3\xaf"
                          "c"
                          "\xc3\xb6"
                          "de/f"
                          "\xc3\xa9"
                          ".txt",
                          noise, 17, TB_PACKAGE_STORED};
    *count = 6;
}

/* Write samples into a buffer stream, which the caller closes. */
static const unsigned char *write_package(const sample *samples, size_t count, const uint8_t *key,
                                          tb_stream *buffer, size_t *length)
{
    tb_package_writer *writer = NULL;
    tb_stream_open_buffer(NULL, buffer);
    TB_TEST_REQUIRE_UINT(tb_package_writer_create(buffer, key, NULL, &writer), ==, TB_PACKAGE_OK);
    for (size_t index = 0; index < count; index++)
    {
        TB_TEST_REQUIRE_UINT(tb_package_add(writer, samples[index].name,
                                            tb_str_length(samples[index].name),
                                            samples[index].bytes, samples[index].size, 6),
                             ==, TB_PACKAGE_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OK);
    tb_package_writer_destroy(writer);
    return tb_stream_buffer_bytes(buffer, length);
}

/* Read an entry whole; return the stream's last result. */
static tb_stream_result read_entry(tb_package *package, size_t index, unsigned char *bytes,
                                   size_t capacity, size_t *length)
{
    tb_stream stream = {0};
    tb_stream_result result = TB_STREAM_OK;
    *length = 0;
    tb_package_open_entry(package, index, NULL, &stream);
    for (;;)
    {
        size_t count = 0;
        result = tb_stream_read(&stream, bytes + *length, capacity - *length, &count);
        *length += count;
        if (result != TB_STREAM_OK || count == 0)
        {
            break;
        }
    }
    (void)tb_stream_close(&stream);
    return result;
}

static void expect_samples(tb_package *package, const sample *samples, size_t count)
{
    static unsigned char data[TEXT_SIZE];
    TB_TEST_REQUIRE_UINT(tb_package_count(package), ==, count);
    for (size_t index = 0; index < count; index++)
    {
        const sample *const want = &samples[index];
        tb_package_entry entry = {0};
        size_t found = 0;
        size_t length = 0;
        TB_TEST_CONTEXT("%s", want->name);
        TB_TEST_REQUIRE(tb_package_find(package, want->name, tb_str_length(want->name), &found));
        tb_package_entry_at(package, found, &entry);
        TB_TEST_EXPECT_STRING(entry.name, ==, want->name);
        TB_TEST_EXPECT_UINT(entry.size, ==, want->size);
        TB_TEST_EXPECT_UINT(entry.codec, ==, want->codec);
        TB_TEST_EXPECT_UINT(entry.crc32, ==, tb_crc32(want->bytes, want->size));
        TB_TEST_EXPECT_UINT(read_entry(package, found, data, sizeof(data), &length), ==,
                            TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(length, ==, want->size);
        if (length == want->size && length > 0)
        {
            TB_TEST_EXPECT_BYTES(data, want->bytes, length);
        }
    }
}

static tb_package_result open_bytes(const void *bytes, size_t length, const uint8_t *key,
                                    tb_stream *source, tb_package **package)
{
    tb_stream_open_memory(bytes, length, NULL, source);
    return tb_package_open(source, key, NULL, NULL, package);
}

TB_TEST(codec_package, entries_read_back_plain_and_obfuscated)
{
    sample samples[8] = {0};
    size_t count = 0;
    make_samples(samples, &count);
    for (size_t pass = 0; pass < 2; pass++)
    {
        const uint8_t *const key = pass == 0 ? NULL : game_key;
        tb_stream buffer = {0};
        tb_stream source = {0};
        tb_package *package = NULL;
        size_t length = 0;
        const unsigned char *const bytes = write_package(samples, count, key, &buffer, &length);
        TB_TEST_CONTEXT("%s", pass == 0 ? "plain" : "obfuscated");
        TB_TEST_REQUIRE_UINT(open_bytes(bytes, length, key, &source, &package), ==, TB_PACKAGE_OK);
        expect_samples(package, samples, count);
        /* Entries are listed by name, and a name not there is not found. */
        for (size_t index = 1; index < tb_package_count(package); index++)
        {
            tb_package_entry before = {0};
            tb_package_entry after = {0};
            tb_package_entry_at(package, index - 1, &before);
            tb_package_entry_at(package, index, &after);
            TB_TEST_EXPECT(tb_str_compare(before.name, after.name) < 0);
        }
        {
            size_t index = 0;
            TB_TEST_EXPECT(!tb_package_find(package, "textures/rock", 13, &index));
            TB_TEST_EXPECT(!tb_package_find(package, "", 0, &index));
        }
        tb_package_close(package);
        (void)tb_stream_close(&source);
        (void)tb_stream_close(&buffer);
    }
}

TB_TEST(codec_package, an_empty_package_has_no_entries)
{
    for (size_t pass = 0; pass < 2; pass++)
    {
        const uint8_t *const key = pass == 0 ? NULL : game_key;
        tb_stream buffer = {0};
        tb_stream source = {0};
        tb_package *package = NULL;
        size_t length = 0;
        const unsigned char *const bytes = write_package(NULL, 0, key, &buffer, &length);
        size_t index = 0;
        TB_TEST_EXPECT_UINT(length, ==, 64 + 48);
        TB_TEST_REQUIRE_UINT(open_bytes(bytes, length, key, &source, &package), ==, TB_PACKAGE_OK);
        TB_TEST_EXPECT_UINT(tb_package_count(package), ==, 0);
        TB_TEST_EXPECT(!tb_package_find(package, "a", 1, &index));
        tb_package_close(package);
        (void)tb_stream_close(&source);
        (void)tb_stream_close(&buffer);
    }
}

TB_TEST(codec_package, thousands_of_entries_are_found_by_name)
{
    enum
    {
        ENTRIES = 5000
    };
    tb_stream buffer = {0};
    tb_stream source = {0};
    tb_package_writer *writer = NULL;
    tb_package *package = NULL;
    const unsigned char *bytes = NULL;
    size_t length = 0;
    char name[32] = {0};
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_REQUIRE_UINT(tb_package_writer_create(&buffer, game_key, NULL, &writer), ==,
                         TB_PACKAGE_OK);
    /* Added in an order that is not the order of names. */
    for (size_t index = 0; index < ENTRIES; index++)
    {
        const size_t number = (index * 7919) % ENTRIES;
        const size_t name_length =
            tb_fmt_buffer(name, sizeof(name), "dir%zu/file%zu", number % 13, number);
        TB_TEST_REQUIRE_UINT(tb_package_add(writer, name, name_length, name, name_length, 0), ==,
                             TB_PACKAGE_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OK);
    tb_package_writer_destroy(writer);
    bytes = tb_stream_buffer_bytes(&buffer, &length);
    TB_TEST_REQUIRE_UINT(open_bytes(bytes, length, game_key, &source, &package), ==, TB_PACKAGE_OK);
    TB_TEST_REQUIRE_UINT(tb_package_count(package), ==, ENTRIES);
    for (size_t number = 0; number < ENTRIES; number++)
    {
        const size_t name_length =
            tb_fmt_buffer(name, sizeof(name), "dir%zu/file%zu", number % 13, number);
        size_t index = 0;
        unsigned char data[32] = {0};
        size_t read = 0;
        TB_TEST_CONTEXT("%s", name);
        TB_TEST_REQUIRE(tb_package_find(package, name, name_length, &index));
        TB_TEST_REQUIRE_UINT(read_entry(package, index, data, sizeof(data), &read), ==,
                             TB_STREAM_OK);
        TB_TEST_REQUIRE_UINT(read, ==, name_length);
        TB_TEST_REQUIRE_BYTES(data, name, name_length);
    }
    tb_package_close(package);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&buffer);
}

static bool contains(const unsigned char *bytes, size_t length, const void *pattern, size_t size)
{
    for (size_t at = 0; at + size <= length; at++)
    {
        if (memcmp(bytes + at, pattern, size) == 0)
        {
            return true;
        }
    }
    return false;
}

/* An obfuscated package shows neither its names nor its data; two packages
 * of the same entries differ, each by its salt; and keys are checked. */
TB_TEST(codec_package, obfuscation_hides_names_and_data_and_checks_keys)
{
    sample samples[8] = {0};
    size_t count = 0;
    tb_stream first = {0};
    tb_stream second = {0};
    tb_stream plain = {0};
    tb_stream source = {0};
    tb_package *package = NULL;
    size_t first_length = 0;
    size_t second_length = 0;
    size_t plain_length = 0;
    const unsigned char *first_bytes = NULL;
    const unsigned char *second_bytes = NULL;
    const unsigned char *plain_bytes = NULL;
    uint8_t wrong_key[TB_PACKAGE_KEY_SIZE] = {0};
    make_samples(samples, &count);
    /* Stored data, so the plain package shows it as it is. */
    samples[0].codec = TB_PACKAGE_STORED;
    plain_bytes = write_package(samples + 1, 1, NULL, &plain, &plain_length);
    TB_TEST_EXPECT(contains(plain_bytes, plain_length, "sounds/noise.bin", 16));
    TB_TEST_EXPECT(contains(plain_bytes, plain_length, noise, 64));
    first_bytes = write_package(samples, count, game_key, &first, &first_length);
    second_bytes = write_package(samples, count, game_key, &second, &second_length);
    TB_TEST_EXPECT(!contains(first_bytes, first_length, "sounds/noise.bin", 16));
    TB_TEST_EXPECT(!contains(first_bytes, first_length, "textures", 8));
    TB_TEST_EXPECT(!contains(first_bytes, first_length, noise, 16));
    TB_TEST_EXPECT_UINT(first_length, ==, second_length);
    TB_TEST_EXPECT(memcmp(first_bytes + 64, second_bytes + 64, first_length - 64 - 48) != 0);
    /* No key, a wrong key, and a key for a plain package. */
    TB_TEST_EXPECT_UINT(open_bytes(first_bytes, first_length, NULL, &source, &package), ==,
                        TB_PACKAGE_KEY_REQUIRED);
    TB_TEST_EXPECT_POINTER(package, ==, NULL);
    (void)tb_stream_close(&source);
    memcpy(wrong_key, game_key, sizeof(wrong_key));
    wrong_key[31] ^= 1u;
    TB_TEST_EXPECT_UINT(open_bytes(first_bytes, first_length, wrong_key, &source, &package), ==,
                        TB_PACKAGE_WRONG_KEY);
    (void)tb_stream_close(&source);
    TB_TEST_REQUIRE_UINT(open_bytes(plain_bytes, plain_length, game_key, &source, &package), ==,
                         TB_PACKAGE_OK);
    tb_package_close(package);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&plain);
    (void)tb_stream_close(&second);
    (void)tb_stream_close(&first);
}

typedef struct reader_job
{
    tb_package *package;
    const sample *samples;
    size_t count;
    size_t failures;
} reader_job;

static void read_everything(void *context)
{
    reader_job *const job = context;
    static _Thread_local unsigned char data[TEXT_SIZE];
    for (size_t round = 0; round < 20; round++)
    {
        for (size_t index = 0; index < job->count; index++)
        {
            const sample *const want = &job->samples[index];
            size_t found = 0;
            size_t length = 0;
            if (!tb_package_find(job->package, want->name, tb_str_length(want->name), &found) ||
                read_entry(job->package, found, data, sizeof(data), &length) != TB_STREAM_OK ||
                length != want->size || (length > 0 && memcmp(data, want->bytes, length) != 0))
            {
                job->failures++;
            }
        }
    }
}

TB_TEST(codec_package, many_threads_read_one_obfuscated_package_at_once)
{
    enum
    {
        THREADS = 8
    };
    sample samples[8] = {0};
    size_t count = 0;
    tb_stream buffer = {0};
    tb_stream source = {0};
    tb_package *package = NULL;
    tb_thread threads[THREADS] = {0};
    reader_job jobs[THREADS] = {0};
    size_t length = 0;
    const unsigned char *bytes = NULL;
    make_samples(samples, &count);
    bytes = write_package(samples, count, game_key, &buffer, &length);
    TB_TEST_REQUIRE_UINT(open_bytes(bytes, length, game_key, &source, &package), ==, TB_PACKAGE_OK);
    for (size_t index = 0; index < THREADS; index++)
    {
        jobs[index] = (reader_job){.package = package, .samples = samples, .count = count};
        tb_thread_create(&threads[index], read_everything, &jobs[index]);
    }
    for (size_t index = 0; index < THREADS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(jobs[index].failures, ==, 0);
    }
    tb_package_close(package);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&buffer);
}

/* -------------------------------------------------------------------------
 * Writing refused.
 * ------------------------------------------------------------------------- */

typedef struct sink
{
    size_t length;
    size_t limit;
} sink;

static tb_stream_result sink_write(void *state, const void *bytes, size_t length,
                                   size_t *transferred)
{
    sink *const output = state;
    (void)bytes;
    *transferred = 0;
    if (output->length >= output->limit)
    {
        return TB_STREAM_NO_SPACE;
    }
    *transferred =
        length < output->limit - output->length ? length : output->limit - output->length;
    output->length += *transferred;
    return TB_STREAM_OK;
}

static tb_stream_result sink_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions sink_functions = {.write_bytes = sink_write,
                                                   .release = sink_release};

TB_TEST(codec_package, names_and_levels_the_writer_cannot_take_are_refused)
{
    static const char *const bad_names[] = {"",   "/a",  "a/",   "a//b", ".",
                                            "..", "./a", "a/..", "a\xff"};
    tb_stream buffer = {0};
    tb_package_writer *writer = NULL;
    static char huge_name[70000];
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_REQUIRE_UINT(tb_package_writer_create(&buffer, NULL, NULL, &writer), ==, TB_PACKAGE_OK);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bad_names); index++)
    {
        TB_TEST_CONTEXT("name %zu", index);
        TB_TEST_EXPECT_UINT(
            tb_package_add(writer, bad_names[index], tb_str_length(bad_names[index]), "x", 1, 0),
            ==, TB_PACKAGE_INVALID);
    }
    memset(huge_name, 'h', sizeof(huge_name));
    TB_TEST_EXPECT_UINT(tb_package_add(writer, huge_name, sizeof(huge_name), "x", 1, 0), ==,
                        TB_PACKAGE_INVALID);
    TB_TEST_EXPECT_UINT(tb_package_add(writer, "a", 1, "x", 1, 10), ==, TB_PACKAGE_INVALID);
    TB_TEST_EXPECT_UINT(tb_package_add(writer, "a", 1, "x", 1, -1), ==, TB_PACKAGE_INVALID);
    TB_TEST_EXPECT_UINT(tb_package_add(writer, "a", 1, "x", 1, 0), ==, TB_PACKAGE_OK);
    TB_TEST_EXPECT_UINT(tb_package_add(writer, "a", 1, "y", 1, 0), ==, TB_PACKAGE_DUPLICATE_NAME);
    TB_TEST_EXPECT_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OK);
    tb_package_writer_destroy(writer);
    (void)tb_stream_close(&buffer);
}

TB_TEST(codec_package, a_sink_that_refuses_is_reported_from_then_on)
{
    for (size_t limit = 0; limit < 400; limit += 37)
    {
        sink state = {.limit = limit};
        tb_stream stream = {&sink_functions, &state};
        tb_package_writer *writer = NULL;
        tb_package_result result = tb_package_writer_create(&stream, game_key, NULL, &writer);
        TB_TEST_CONTEXT("limit %zu", limit);
        if (limit < 64)
        {
            TB_TEST_EXPECT_UINT(result, ==, TB_PACKAGE_OUTPUT_FAILED);
            TB_TEST_EXPECT_POINTER(writer, ==, NULL);
            continue;
        }
        TB_TEST_REQUIRE_UINT(result, ==, TB_PACKAGE_OK);
        result = tb_package_add(writer, "one", 3, noise, 200, 0);
        if (result == TB_PACKAGE_OK)
        {
            result = tb_package_add(writer, "two", 3, noise, 200, 0);
        }
        TB_TEST_EXPECT_UINT(tb_package_writer_finish(writer), ==, TB_PACKAGE_OUTPUT_FAILED);
        tb_package_writer_destroy(writer);
    }
}

/* -------------------------------------------------------------------------
 * Damaged and inconsistent packages.
 * ------------------------------------------------------------------------- */

typedef struct copy
{
    unsigned char bytes[64 * 1024];
    size_t length;
} copy;

static void put32(unsigned char *bytes, uint32_t value)
{
    for (size_t index = 0; index < 4; index++)
    {
        bytes[index] = (unsigned char)((value >> (8 * index)) & 0xFFu);
    }
}

static void put64(unsigned char *bytes, uint64_t value)
{
    put32(bytes, (uint32_t)(value & 0xFFFFFFFFu));
    put32(bytes + 4, (uint32_t)(value >> 32));
}

static uint64_t get64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (size_t index = 8; index > 0; index--)
    {
        value = (value << 8) | bytes[index - 1];
    }
    return value;
}

/* A plain package of the samples, to damage. */
static void make_copy(copy *output)
{
    sample samples[8] = {0};
    size_t count = 0;
    tb_stream buffer = {0};
    size_t length = 0;
    const unsigned char *bytes = NULL;
    make_samples(samples, &count);
    bytes = write_package(samples, count, NULL, &buffer, &length);
    TB_TEST_REQUIRE_UINT(length, <=, sizeof(output->bytes));
    memcpy(output->bytes, bytes, length);
    output->length = length;
    (void)tb_stream_close(&buffer);
}

static unsigned char *index_of(copy *input, size_t *size)
{
    unsigned char *const footer = input->bytes + input->length - 48;
    *size = (size_t)get64(footer + 8);
    return input->bytes + get64(footer);
}

/* After a change to the index, make its CRC-32 right again, so the change
 * itself is what the reader must catch. */
static void reseal(copy *input)
{
    size_t size = 0;
    const unsigned char *const index = index_of(input, &size);
    put32(input->bytes + input->length - 48 + 24, tb_crc32(index, size));
}

/* Open and read every entry; return the first failure, as a package result
 * or, for an entry's data, MALFORMED. */
static tb_package_result open_and_read(const unsigned char *bytes, size_t length,
                                       const uint8_t *key, const tb_package_options *options)
{
    static unsigned char data[TEXT_SIZE + 1];
    tb_stream source = {0};
    tb_package *package = NULL;
    tb_package_result result = TB_PACKAGE_OK;
    tb_stream_open_memory(bytes, length, NULL, &source);
    result = tb_package_open(&source, key, options, NULL, &package);
    for (size_t index = 0; result == TB_PACKAGE_OK && index < tb_package_count(package); index++)
    {
        size_t read = 0;
        if (read_entry(package, index, data, sizeof(data), &read) != TB_STREAM_OK)
        {
            result = TB_PACKAGE_MALFORMED;
        }
    }
    tb_package_close(package);
    (void)tb_stream_close(&source);
    return result;
}

TB_TEST(codec_package, every_truncation_is_refused)
{
    static copy input;
    make_copy(&input);
    for (size_t cut = 0; cut < input.length;
         cut += (cut < 200 || cut > input.length - 400) ? 1 : 97)
    {
        TB_TEST_CONTEXT("cut at %zu", cut);
        TB_TEST_EXPECT_UINT(open_and_read(input.bytes, cut, NULL, NULL), !=, TB_PACKAGE_OK);
    }
}

TB_TEST(codec_package, damaged_headers_footers_and_indexes_are_refused)
{
    static copy input;
    unsigned char *index = NULL;
    size_t size = 0;
    /* The magic, a newer version, an unknown flag. */
    make_copy(&input);
    input.bytes[0] = 'X';
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    input.bytes[8] = 2;
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_UNSUPPORTED);
    make_copy(&input);
    input.bytes[10] = 2;
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_UNSUPPORTED);
    /* The footer's magic, and an index outside the package. */
    make_copy(&input);
    input.bytes[input.length - 1] = 'X';
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    put64(input.bytes + input.length - 48, input.length);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    put64(input.bytes + input.length - 48 + 16, 1000000);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    /* An index whose CRC-32 does not match. */
    make_copy(&input);
    index = index_of(&input, &size);
    index[size - 1] ^= 1u;
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    /* Records that lie, with the CRC-32 made right: data outside the package,
     * data that overlaps, a stored size that differs, an unknown codec,
     * names out of order, and a name that climbs out. The first record is
     * "a", 1 byte, stored. */
    make_copy(&input);
    index = index_of(&input, &size);
    put64(index + 8, input.length);
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    index = index_of(&input, &size);
    /* "a" moved onto the data of "sounds/noise.bin", the third record, after
     * "a" (32 + 1 bytes) and "empty" (32 + 5). */
    put64(index + 8, get64(index + 33 + 37 + 8));
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    index = index_of(&input, &size);
    put64(index + 24, 2);
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    index = index_of(&input, &size);
    index[2] = 7;
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    index = index_of(&input, &size);
    index[32] = 'z';
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    make_copy(&input);
    index = index_of(&input, &size);
    index[32] = '/';
    reseal(&input);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
    /* A limit on entries. */
    make_copy(&input);
    TB_TEST_EXPECT_UINT(
        open_and_read(input.bytes, input.length, NULL, &(tb_package_options){.max_entries = 5}), ==,
        TB_PACKAGE_TOO_LARGE);
}

TB_TEST(codec_package, damaged_data_is_malformed_when_read)
{
    static copy input;
    make_copy(&input);
    /* The deflated text begins after the header, the first entry being "a"
     * at 64 and the empty one at 80; damage every entry's data a little. */
    for (size_t at = 64; at < input.length - 48 - 200; at += 512)
    {
        input.bytes[at] ^= 0x55u;
    }
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL, NULL), ==,
                        TB_PACKAGE_MALFORMED);
}

TB_TEST(codec_package, mutated_packages_are_survived)
{
    static copy input;
    for (size_t trial = 0; trial < 1000; trial++)
    {
        const bool obfuscated = (trial & 1u) != 0;
        sample samples[8] = {0};
        size_t count = 0;
        tb_stream buffer = {0};
        size_t length = 0;
        const unsigned char *bytes = NULL;
        tb_package_result result = TB_PACKAGE_OK;
        TB_TEST_CONTEXT("trial %zu", trial);
        make_samples(samples, &count);
        bytes = write_package(samples, count, obfuscated ? game_key : NULL, &buffer, &length);
        memcpy(input.bytes, bytes, length);
        (void)tb_stream_close(&buffer);
        for (size_t change = 0; change < 3; change++)
        {
            input.bytes[tb_test_random_below(length)] ^=
                (unsigned char)(1u + tb_test_random_below(255));
        }
        result = open_and_read(input.bytes, length, obfuscated ? game_key : NULL, NULL);
        TB_TEST_EXPECT(result != TB_PACKAGE_INVALID && result != TB_PACKAGE_INPUT_FAILED &&
                       result != TB_PACKAGE_OUTPUT_FAILED && result != TB_PACKAGE_KEY_REQUIRED);
    }
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void package_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static unsigned char memory[256 * 1024];
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_stream buffer = {0};
    tb_stream source = {0};
    tb_package_writer *writer = NULL;
    tb_package *package = NULL;
    tb_package_entry entry = {0};
    tb_stream data = {0};
    tb_stream_open_buffer(&allocator, &buffer);
    switch (choice)
    {
    case 0:
        (void)tb_package_writer_create(NULL, NULL, &allocator, &writer);
        break;
    case 1:
        (void)tb_package_writer_create(&buffer, NULL, &allocator, &writer);
        (void)tb_package_add(writer, "a", 1, NULL, 1, 0);
        break;
    case 2:
        (void)tb_package_writer_create(&buffer, NULL, &allocator, &writer);
        (void)tb_package_writer_finish(writer);
        (void)tb_package_add(writer, "a", 1, "x", 1, 0);
        break;
    case 3:
        (void)tb_package_open(NULL, NULL, NULL, &allocator, &package);
        break;
    default: {
        const unsigned char *bytes = NULL;
        size_t length = 0;
        (void)tb_package_writer_create(&buffer, NULL, &allocator, &writer);
        (void)tb_package_writer_finish(writer);
        bytes = tb_stream_buffer_bytes(&buffer, &length);
        tb_stream_open_memory(bytes, length, &allocator, &source);
        (void)tb_package_open(&source, NULL, NULL, &allocator, &package);
        if (choice == 4)
        {
            tb_package_entry_at(package, 0, &entry);
        }
        else
        {
            tb_package_open_entry(package, 0, &allocator, &data);
        }
        break;
    }
    }
    tb_package_close(package);
    tb_package_writer_destroy(writer);
    tb_arena_destroy(arena);
}

TB_TEST(codec_package, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 6; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(package_contract, &choice, "assertion failed");
    }
}
