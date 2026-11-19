/* ZIP reading and writing: archives that Python's zipfile, bsdtar, and .NET
 * wrote; the writer's own archives read back, past every 16- and 32-bit limit
 * of the format; and damaged, inconsistent, and hostile archives, which must
 * be refused or reported, never trusted. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/fmt.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/zip.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdbool.h>
#include <string.h>

extern const tb_test_file tb_zip_files[];
extern const size_t tb_zip_files_count;

#define MODIFIED INT64_C(1700000000)

/* -------------------------------------------------------------------------
 * The entries of the reference archives, as tools/make_zip_test_data.py
 * writes them.
 * ------------------------------------------------------------------------- */

enum
{
    NEEDS_UTF8_NAMES = 1, /* Not in bsdtar's, which garbles them on Windows. */
    NEEDS_UNIX = 2        /* Not in .NET's, which keeps no modes or links. */
};

typedef enum content
{
    CONTENT_TEXT,
    CONTENT_STRIDE,   /* 1000 bytes of (index * 7 + 3) & 0xFF. */
    CONTENT_COUNTING, /* 512 bytes of index & 0xFF. */
    CONTENT_LINES     /* 2500 numbered lines. */
} content;

typedef struct expected_entry
{
    const char *name;
    tb_zip_type type;
    uint32_t mode;
    tb_zip_method method;
    content data;
    const char *text;
    unsigned needs;
} expected_entry;

#define UTF8_NAME                                                                                  \
    "\xc3\xbc"                                                                                     \
    "n"                                                                                            \
    "\xc3\xaf"                                                                                     \
    "c"                                                                                            \
    "\xc3\xb6"                                                                                     \
    "d"                                                                                            \
    "\xc3\xa9"                                                                                     \
    ".txt"
#define UTF8_TEXT                                                                                  \
    "\xc3\xbc"                                                                                     \
    "n"                                                                                            \
    "\xc3\xaf"                                                                                     \
    "c"                                                                                            \
    "\xc3\xb6"                                                                                     \
    "d"                                                                                            \
    "\xc3\xa9"                                                                                     \
    "\n"

static const expected_entry expected_entries[] = {
    {"hello.txt", TB_ZIP_FILE, 0644, TB_ZIP_DEFLATED, CONTENT_TEXT, "Hello, zip!\n", 0},
    {"empty.txt", TB_ZIP_FILE, 0644, TB_ZIP_STORED, CONTENT_TEXT, "", 0},
    {"dir", TB_ZIP_DIRECTORY, 0755, TB_ZIP_STORED, CONTENT_TEXT, "", 0},
    {"dir/data.bin", TB_ZIP_FILE, 0644, TB_ZIP_DEFLATED, CONTENT_STRIDE, NULL, 0},
    {"stored.bin", TB_ZIP_FILE, 0600, TB_ZIP_STORED, CONTENT_COUNTING, NULL, 0},
    {"big.txt", TB_ZIP_FILE, 0644, TB_ZIP_DEFLATED, CONTENT_LINES, NULL, 0},
    {UTF8_NAME, TB_ZIP_FILE, 0644, TB_ZIP_DEFLATED, CONTENT_TEXT, UTF8_TEXT, NEEDS_UTF8_NAMES},
    {"run.sh", TB_ZIP_FILE, 0755, TB_ZIP_DEFLATED, CONTENT_TEXT, "#!/bin/sh\necho run\n", 0},
    {"link", TB_ZIP_SYMLINK, 0777, TB_ZIP_STORED, CONTENT_TEXT, "hello.txt", NEEDS_UNIX},
};

#define LINES_SIZE ((size_t)105000)

/* The entry's data, into `bytes` of room for LINES_SIZE; returns its length. */
static size_t content_of(const expected_entry *entry, unsigned char *bytes)
{
    size_t length = 0;
    switch (entry->data)
    {
    case CONTENT_STRIDE:
        length = 1000;
        for (size_t index = 0; index < length; index++)
        {
            bytes[index] = (unsigned char)((index * 7 + 3) & 0xFFu);
        }
        break;
    case CONTENT_COUNTING:
        length = 512;
        for (size_t index = 0; index < length; index++)
        {
            bytes[index] = (unsigned char)(index & 0xFFu);
        }
        break;
    case CONTENT_LINES:
        for (size_t line = 0; line < 2500; line++)
        {
            length += tb_fmt_buffer((char *)bytes + length, LINES_SIZE + 1 - length,
                                    "line %05zu of a text that compresses well\n", line);
        }
        break;
    case CONTENT_TEXT:
    default:
        length = tb_str_length(entry->text);
        memcpy(bytes, entry->text, length);
        break;
    }
    return length;
}

static unsigned missing_from(const char *file)
{
    if (tb_str_starts_with(file, "bsdtar"))
    {
        return NEEDS_UTF8_NAMES;
    }
    if (tb_str_starts_with(file, "dotnet"))
    {
        return NEEDS_UNIX;
    }
    return 0;
}

static const tb_test_file *find_file(const char *name)
{
    const tb_test_file *found = NULL;
    for (size_t index = 0; index < tb_zip_files_count; index++)
    {
        if (tb_str_equal(tb_zip_files[index].name, name))
        {
            found = &tb_zip_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    return found;
}

/* Read an entry whole into `bytes`; return the stream's last result. */
static tb_stream_result read_entry(tb_zip_archive *archive, size_t index, unsigned char *bytes,
                                   size_t capacity, size_t *length)
{
    tb_stream stream = {0};
    tb_stream_result result = TB_STREAM_OK;
    *length = 0;
    TB_TEST_REQUIRE_UINT(tb_zip_open_entry(archive, index, NULL, &stream), ==, TB_ZIP_OK);
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

/* Require an open archive to hold exactly the expected entries but those in
 * `missing`, in order; `unix` says whether it keeps modes. */
static void expect_entries(tb_zip_archive *archive, unsigned missing, bool unix)
{
    static unsigned char data[LINES_SIZE + 1];
    static unsigned char expected[LINES_SIZE + 1];
    size_t position = 0;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
    {
        const expected_entry *const want = &expected_entries[index];
        tb_zip_entry entry = {0};
        size_t found = 0;
        size_t length = 0;
        size_t expected_length = 0;
        if ((want->needs & missing) != 0)
        {
            continue;
        }
        TB_TEST_CONTEXT("entry %zu", index);
        TB_TEST_REQUIRE_UINT(position, <, tb_zip_count(archive));
        tb_zip_entry_at(archive, position, &entry);
        TB_TEST_EXPECT_STRING(entry.name, ==, want->name);
        TB_TEST_EXPECT_UINT(entry.name_length, ==, tb_str_length(want->name));
        TB_TEST_EXPECT(tb_zip_find(archive, want->name, tb_str_length(want->name), &found));
        TB_TEST_EXPECT_UINT(found, ==, position);
        TB_TEST_EXPECT_UINT(entry.type, ==, want->type);
        TB_TEST_EXPECT_UINT(entry.mode, ==, unix ? want->mode : 0u);
        TB_TEST_EXPECT_INT(entry.modified, ==, MODIFIED);
        TB_TEST_EXPECT(!entry.encrypted);
        expected_length = content_of(want, expected);
        TB_TEST_EXPECT_UINT(entry.size, ==, expected_length);
        TB_TEST_EXPECT_UINT(read_entry(archive, position, data, sizeof(data), &length), ==,
                            TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(length, ==, expected_length);
        TB_TEST_EXPECT_BYTES(data, expected, expected_length);
        position++;
    }
    TB_TEST_EXPECT_UINT(tb_zip_count(archive), ==, position);
}

static tb_zip_result open_bytes(const void *bytes, size_t length, const tb_zip_options *options,
                                tb_stream *source, tb_zip_archive **archive)
{
    tb_stream_open_memory(bytes, length, NULL, source);
    return tb_zip_open(source, options, NULL, archive);
}

/* -------------------------------------------------------------------------
 * Reading what other tools wrote.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_zip, archives_of_three_writers_read_exactly)
{
    static const char *const archives[] = {
        "python.zip",       "python-zip64.zip", "python-comments.zip",
        "python-extra.zip", "bsdtar.zip",       "dotnet.zip",
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(archives); index++)
    {
        const tb_test_file *const file = find_file(archives[index]);
        tb_stream source = {0};
        tb_zip_archive *archive = NULL;
        TB_TEST_CONTEXT("%s", file->name);
        TB_TEST_REQUIRE_UINT(open_bytes(file->bytes, file->length, NULL, &source, &archive), ==,
                             TB_ZIP_OK);
        expect_entries(archive, missing_from(file->name),
                       !tb_str_starts_with(file->name, "dotnet"));
        tb_zip_close(archive);
        (void)tb_stream_close(&source);
    }
}

TB_TEST(codec_zip, names_in_code_page_437_read_as_utf8)
{
    const tb_test_file *const file = find_file("python-legacy.zip");
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    tb_zip_entry entry = {0};
    unsigned char data[16] = {0};
    size_t length = 0;
    size_t index = 0;
    TB_TEST_REQUIRE_UINT(open_bytes(file->bytes, file->length, NULL, &source, &archive), ==,
                         TB_ZIP_OK);
    TB_TEST_REQUIRE_UINT(tb_zip_count(archive), ==, 1);
    tb_zip_entry_at(archive, 0, &entry);
    TB_TEST_EXPECT_STRING(entry.name, ==, "caf\xc3\xa9.txt");
    TB_TEST_EXPECT(tb_zip_find(archive, "caf\xc3\xa9.txt", 9, &index));
    TB_TEST_EXPECT_UINT(read_entry(archive, 0, data, sizeof(data), &length), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(length, ==, 7);
    TB_TEST_EXPECT_BYTES(data, "legacy\n", 7);
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
}

TB_TEST(codec_zip, an_empty_archive_has_no_entries)
{
    const tb_test_file *const file = find_file("python-empty.zip");
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    size_t index = 0;
    TB_TEST_REQUIRE_UINT(open_bytes(file->bytes, file->length, NULL, &source, &archive), ==,
                         TB_ZIP_OK);
    TB_TEST_EXPECT_UINT(tb_zip_count(archive), ==, 0);
    TB_TEST_EXPECT(!tb_zip_find(archive, "a", 1, &index));
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
}

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/* Takes written bytes into a buffer, at most `step` at a time, and refuses
 * any past `limit`. */
typedef struct sink
{
    unsigned char *bytes;
    size_t capacity;
    size_t length;
    size_t step;
    size_t limit;
} sink;

static tb_stream_result sink_write(void *state, const void *bytes, size_t length,
                                   size_t *transferred)
{
    sink *const output = state;
    size_t count = length < output->step ? length : output->step;
    *transferred = 0;
    if (output->length >= output->limit)
    {
        return TB_STREAM_NO_SPACE;
    }
    if (count > output->limit - output->length)
    {
        count = output->limit - output->length;
    }
    TB_TEST_REQUIRE_UINT(count, <=, output->capacity - output->length);
    memcpy(output->bytes + output->length, bytes, count);
    output->length += count;
    *transferred = count;
    return TB_STREAM_OK;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions sink_functions = {
    .write_bytes = sink_write,
    .release = nothing_to_release,
};

static tb_stream open_sink(sink *state, unsigned char *bytes, size_t capacity, size_t step,
                           size_t limit)
{
    const tb_stream stream = {&sink_functions, state};
    *state = (sink){.bytes = bytes, .capacity = capacity, .step = step, .limit = limit};
    return stream;
}

static tb_zip_entry entry_of(const char *name, tb_zip_type type, tb_zip_method method,
                             uint64_t size)
{
    return (tb_zip_entry){
        .name = name,
        .name_length = tb_str_length(name),
        .type = type,
        .method = (uint16_t)method,
        .size = size,
        .modified = MODIFIED,
    };
}

/* Write the expected entries but the link, which the writer does not write. */
static void write_expected(tb_zip_writer *writer)
{
    static unsigned char data[LINES_SIZE + 1];
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
    {
        const expected_entry *const want = &expected_entries[index];
        const size_t length = content_of(want, data);
        tb_zip_entry entry = entry_of(want->name, want->type, want->method, length);
        if (want->type == TB_ZIP_SYMLINK)
        {
            continue;
        }
        entry.mode = want->mode;
        TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 6), ==, TB_ZIP_OK);
        for (size_t at = 0; at < length; at += 7000)
        {
            const size_t piece = length - at < 7000 ? length - at : 7000;
            TB_TEST_REQUIRE_UINT(tb_zip_write_data(writer, data + at, piece), ==, TB_ZIP_OK);
        }
    }
    TB_TEST_REQUIRE_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
}

static size_t write_expected_archive(unsigned char *bytes, size_t capacity, size_t step)
{
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, capacity, step, SIZE_MAX);
    tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
    write_expected(writer);
    tb_zip_writer_destroy(writer);
    return state.length;
}

TB_TEST(codec_zip, the_writer_writes_what_the_reader_reads)
{
    static unsigned char archive_bytes[1 << 16];
    static unsigned char again[1 << 16];
    const size_t length = write_expected_archive(archive_bytes, sizeof(archive_bytes), SIZE_MAX);
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    TB_TEST_REQUIRE_UINT(open_bytes(archive_bytes, length, NULL, &source, &archive), ==, TB_ZIP_OK);
    expect_entries(archive, NEEDS_UNIX, true);
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
    /* The same entries make the same bytes, whatever the sink takes at a time. */
    TB_TEST_EXPECT_UINT(write_expected_archive(again, sizeof(again), 1), ==, length);
    TB_TEST_EXPECT_BYTES(again, archive_bytes, length);
}

TB_TEST(codec_zip, times_round_trip_where_the_format_can_hold_them)
{
    static unsigned char bytes[4096];
    static const struct
    {
        int64_t written;
        int64_t read;
    } times[] = {
        {MODIFIED, MODIFIED},
        {MODIFIED + 1, MODIFIED + 1}, /* Odd seconds survive in the extended timestamp. */
        {0, 0},
        {-1, -1},
        {INT32_MAX, INT32_MAX},
        {INT32_MIN, INT32_MIN},
        /* Past 32 bits only the DOS time is written, at its last second. */
        {INT64_C(5000000000), INT64_C(4354819198)},
        {INT64_MIN, INT64_C(315532800)},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(times); index++)
    {
        sink state = {0};
        tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
        tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
        tb_zip_entry entry = entry_of("time", TB_ZIP_DIRECTORY, TB_ZIP_STORED, 0);
        tb_stream source = {0};
        tb_zip_archive *archive = NULL;
        TB_TEST_CONTEXT("time %zu", index);
        entry.modified = times[index].written;
        TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_OK);
        TB_TEST_REQUIRE_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
        tb_zip_writer_destroy(writer);
        TB_TEST_REQUIRE_UINT(open_bytes(bytes, state.length, NULL, &source, &archive), ==,
                             TB_ZIP_OK);
        tb_zip_entry_at(archive, 0, &entry);
        TB_TEST_EXPECT_INT(entry.modified, ==, times[index].read);
        tb_zip_close(archive);
        (void)tb_stream_close(&source);
    }
}

/* More than 65,535 entries need the ZIP64 end records. */
TB_TEST(codec_zip, more_entries_than_sixteen_bits_count)
{
    enum
    {
        ENTRIES = 65537
    };
    static unsigned char bytes[12 << 20];
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
    tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    tb_zip_entry entry = {0};
    char name[16] = {0};
    size_t index = 0;
    for (size_t count = 0; count < ENTRIES; count++)
    {
        entry = entry_of(name, TB_ZIP_FILE, TB_ZIP_STORED, 1);
        entry.name_length = tb_fmt_buffer(name, sizeof(name), "e%05zu", count);
        TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_OK);
        TB_TEST_REQUIRE_UINT(tb_zip_write_data(writer, name + 5, 1), ==, TB_ZIP_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
    tb_zip_writer_destroy(writer);
    TB_TEST_REQUIRE_UINT(open_bytes(bytes, state.length, NULL, &source, &archive), ==, TB_ZIP_OK);
    TB_TEST_EXPECT_UINT(tb_zip_count(archive), ==, ENTRIES);
    TB_TEST_REQUIRE(tb_zip_find(archive, "e65536", 6, &index));
    TB_TEST_EXPECT_UINT(index, ==, 65536);
    {
        unsigned char data[4] = {0};
        size_t length = 0;
        TB_TEST_EXPECT_UINT(read_entry(archive, index, data, sizeof(data), &length), ==,
                            TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(length, ==, 1);
        TB_TEST_EXPECT_UINT(data[0], ==, (unsigned)'6');
    }
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
    /* A limit lower than the count refuses the archive. */
    TB_TEST_EXPECT_UINT(
        open_bytes(bytes, state.length, &(tb_zip_options){.max_entries = 65536}, &source, &archive),
        ==, TB_ZIP_TOO_LARGE);
    (void)tb_stream_close(&source);
}

/* An archive of headers around 5 GiB of zeros that are never stored: the
 * source makes them up as they are read. */
typedef struct sparse
{
    const unsigned char *head; /* The local header, at 0. */
    size_t head_length;
    uint64_t zeros;
    const unsigned char *tail; /* The central directory and end records, after the zeros. */
    size_t tail_length;
} sparse;

static tb_stream_result sparse_read_at(void *state, uint64_t offset, void *bytes, size_t capacity,
                                       size_t *transferred)
{
    const sparse *const source = state;
    const uint64_t total = source->head_length + source->zeros + source->tail_length;
    unsigned char *const output = bytes;
    size_t count = 0;
    while (count < capacity && offset + count < total)
    {
        const uint64_t at = offset + count;
        if (at < source->head_length)
        {
            output[count] = source->head[at];
        }
        else if (at < source->head_length + source->zeros)
        {
            output[count] = 0;
        }
        else
        {
            output[count] = source->tail[at - source->head_length - source->zeros];
        }
        count++;
    }
    *transferred = count;
    return TB_STREAM_OK;
}

static tb_stream_result sparse_size(void *state, uint64_t *size)
{
    const sparse *const source = state;
    *size = source->head_length + source->zeros + source->tail_length;
    return TB_STREAM_OK;
}

static const tb_stream_functions sparse_functions = {
    .size = sparse_size,
    .read_bytes_at = sparse_read_at,
    .release = nothing_to_release,
};

static void put16(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *bytes, uint64_t value)
{
    for (size_t index = 0; index < 4; index++)
    {
        bytes[index] = (unsigned char)((value >> (8 * index)) & 0xFFu);
    }
}

static void put64(unsigned char *bytes, uint64_t value)
{
    put32(bytes, value & 0xFFFFFFFFu);
    put32(bytes + 4, value >> 32);
}

TB_TEST(codec_zip, sizes_past_four_gibibytes_read_from_zip64_fields)
{
    const uint64_t size = UINT64_C(5) << 30;
    unsigned char head[30 + 3] = {0};
    unsigned char tail[46 + 3 + 28 + 56 + 20 + 22] = {0};
    unsigned char *const central = tail;
    unsigned char *const end64 = tail + 46 + 3 + 28;
    sparse state = {.head = head,
                    .head_length = sizeof(head),
                    .zeros = size,
                    .tail = tail,
                    .tail_length = sizeof(tail)};
    tb_stream source = {&sparse_functions, &state};
    tb_zip_archive *archive = NULL;
    tb_zip_entry entry = {0};
    tb_stream data = {0};
    unsigned char bytes[4096] = {0};
    const uint64_t directory_offset = sizeof(head) + size;
    /* The local header: stored, with the sizes in the central directory only. */
    put32(head, 0x04034B50u);
    put16(head + 4, 45);
    put32(head + 18, 0xFFFFFFFFu);
    put32(head + 22, 0xFFFFFFFFu);
    put16(head + 26, 3);
    memcpy(head + 30, "big", 3);
    /* Its CRC-32 is not checked: the test reads only the start. */
    put32(central, 0x02014B50u);
    put16(central + 4, 45);
    put16(central + 6, 45);
    put32(central + 20, 0xFFFFFFFFu);
    put32(central + 24, 0xFFFFFFFFu);
    put16(central + 28, 3);
    put16(central + 30, 28);
    memcpy(central + 46, "big", 3);
    put16(central + 49, 0x0001);
    put16(central + 51, 24);
    put64(central + 53, size);
    put64(central + 61, size);
    put64(central + 69, 0);
    put32(central + 42, 0xFFFFFFFFu);
    put32(end64, 0x06064B50u);
    put64(end64 + 4, 44);
    put64(end64 + 24, 1);
    put64(end64 + 32, 1);
    put64(end64 + 40, 46 + 3 + 28);
    put64(end64 + 48, directory_offset);
    put32(end64 + 56, 0x07064B50u);
    put64(end64 + 64, directory_offset + 46 + 3 + 28);
    put32(end64 + 72, 1);
    put32(end64 + 76, 0x06054B50u);
    put16(end64 + 84, 0xFFFF);
    put16(end64 + 86, 0xFFFF);
    put32(end64 + 88, 0xFFFFFFFFu);
    put32(end64 + 92, 0xFFFFFFFFu);
    TB_TEST_REQUIRE_UINT(tb_zip_open(&source, NULL, NULL, &archive), ==, TB_ZIP_OK);
    tb_zip_entry_at(archive, 0, &entry);
    TB_TEST_EXPECT_STRING(entry.name, ==, "big");
    TB_TEST_EXPECT_UINT(entry.size, ==, size);
    TB_TEST_EXPECT_UINT(entry.compressed_size, ==, size);
    TB_TEST_REQUIRE_UINT(tb_zip_open_entry(archive, 0, NULL, &data), ==, TB_ZIP_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_read_exact(&data, bytes, sizeof(bytes)), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(bytes[0], ==, 0);
    (void)tb_stream_close(&data);
    tb_zip_close(archive);
}

TB_TEST(codec_zip, the_writer_uses_zip64_fields_for_large_files)
{
    static unsigned char bytes[4096];
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
    tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
    const tb_zip_entry entry = entry_of("large", TB_ZIP_FILE, TB_ZIP_DEFLATED, UINT64_C(5) << 30);
    TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 1), ==, TB_ZIP_OK);
    /* The local header says its sizes are in the ZIP64 field and the descriptor. */
    TB_TEST_EXPECT_UINT(bytes[4], ==, 45);
    TB_TEST_EXPECT_BYTES(bytes + 18, "\xff\xff\xff\xff\xff\xff\xff\xff", 8);
    TB_TEST_EXPECT_UINT(state.length, ==, 30 + 5 + 9 + 20);
    TB_TEST_EXPECT_BYTES(bytes + 30 + 5 + 9, "\x01\x00\x10\x00", 4);
    /* Left unfinished, with its data owed. */
    tb_zip_writer_destroy(writer);
}

TB_TEST(codec_zip, entries_the_writer_cannot_write_are_refused_whole)
{
    static unsigned char bytes[4096];
    static const char *const bad_names[] = {
        "", "/a", "a/", "a//b", ".", "..", "./a", "a/..", "../a", "a\xff",
    };
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
    tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
    tb_zip_entry entry = {0};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bad_names); index++)
    {
        TB_TEST_CONTEXT("name %zu", index);
        entry = entry_of(bad_names[index], TB_ZIP_FILE, TB_ZIP_STORED, 0);
        TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    }
    entry = entry_of("a", TB_ZIP_SYMLINK, TB_ZIP_STORED, 0);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    entry = entry_of("a", TB_ZIP_FILE, (tb_zip_method)12, 0);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    entry = entry_of("a", TB_ZIP_DIRECTORY, TB_ZIP_STORED, 1);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    entry = entry_of("a", TB_ZIP_FILE, TB_ZIP_STORED, 0);
    entry.mode = 010000;
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    entry = entry_of("a", TB_ZIP_FILE, TB_ZIP_DEFLATED, 0);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 10), ==, TB_ZIP_INVALID);
    TB_TEST_EXPECT_UINT(state.length, ==, 0);
    /* A name once written is not written again, as a file or a directory. */
    entry = entry_of("a", TB_ZIP_FILE, TB_ZIP_STORED, 0);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_OK);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    entry = entry_of("a", TB_ZIP_DIRECTORY, TB_ZIP_STORED, 0);
    TB_TEST_EXPECT_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_INVALID);
    TB_TEST_EXPECT_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
    tb_zip_writer_destroy(writer);
}

TB_TEST(codec_zip, a_sink_that_refuses_is_reported_from_then_on)
{
    static unsigned char archive_bytes[1 << 16];
    static unsigned char out[1 << 16];
    static unsigned char data[LINES_SIZE + 1];
    const size_t length = write_expected_archive(archive_bytes, sizeof(archive_bytes), SIZE_MAX);
    for (size_t limit = 0; limit < length; limit += 397)
    {
        sink state = {0};
        tb_stream stream = open_sink(&state, out, sizeof(out), SIZE_MAX, limit);
        tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
        bool failed = false;
        TB_TEST_CONTEXT("limit %zu", limit);
        for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
        {
            const expected_entry *const want = &expected_entries[index];
            const size_t size = content_of(want, data);
            tb_zip_entry entry = entry_of(want->name, want->type, want->method, size);
            tb_zip_result result = TB_ZIP_OK;
            if (want->type == TB_ZIP_SYMLINK)
            {
                continue;
            }
            entry.mode = want->mode;
            result = tb_zip_write_entry(writer, &entry, 6);
            if (result == TB_ZIP_OK)
            {
                result = tb_zip_write_data(writer, data, size);
            }
            else
            {
                TB_TEST_EXPECT_UINT(tb_zip_write_data(writer, data, size), ==,
                                    TB_ZIP_OUTPUT_FAILED);
            }
            TB_TEST_EXPECT(!failed || result == TB_ZIP_OUTPUT_FAILED);
            failed = failed || result == TB_ZIP_OUTPUT_FAILED;
        }
        TB_TEST_EXPECT_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OUTPUT_FAILED);
        TB_TEST_EXPECT_UINT(state.length, ==, limit);
        TB_TEST_EXPECT_BYTES(out, archive_bytes, limit);
        tb_zip_writer_destroy(writer);
    }
}

/* -------------------------------------------------------------------------
 * Damaged, inconsistent, and hostile archives.
 * ------------------------------------------------------------------------- */

/* A copy of an archive to damage. */
typedef struct copy
{
    unsigned char bytes[1 << 16];
    size_t length;
} copy;

static void copy_of(copy *output, const char *name)
{
    const tb_test_file *const file = find_file(name);
    TB_TEST_REQUIRE_UINT(file->length, <=, sizeof(output->bytes));
    memcpy(output->bytes, file->bytes, file->length);
    output->length = file->length;
}

static size_t find_bytes(const copy *input, size_t from, const void *pattern, size_t length)
{
    size_t found = SIZE_MAX;
    for (size_t at = from; at + length <= input->length && found == SIZE_MAX; at++)
    {
        if (memcmp(input->bytes + at, pattern, length) == 0)
        {
            found = at;
        }
    }
    TB_TEST_REQUIRE_UINT(found, !=, SIZE_MAX);
    return found;
}

/* Replace every occurrence of a name with another of its length. */
static size_t replace_all(copy *input, const char *from, const char *to, size_t length)
{
    size_t replaced = 0;
    for (size_t at = 0; at + length <= input->length; at++)
    {
        if (memcmp(input->bytes + at, from, length) == 0)
        {
            memcpy(input->bytes + at, to, length);
            replaced++;
        }
    }
    return replaced;
}

/* Open and read every entry; return the first failure, as a ZIP result or,
 * for an entry's data, MALFORMED. */
static tb_zip_result open_and_read(const unsigned char *bytes, size_t length,
                                   const tb_zip_options *options)
{
    static unsigned char data[1 << 17];
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    tb_zip_result result = open_bytes(bytes, length, options, &source, &archive);
    for (size_t index = 0; result == TB_ZIP_OK && index < tb_zip_count(archive); index++)
    {
        tb_stream stream = {0};
        result = tb_zip_open_entry(archive, index, NULL, &stream);
        while (result == TB_ZIP_OK)
        {
            size_t count = 0;
            const tb_stream_result read = tb_stream_read(&stream, data, sizeof(data), &count);
            if (read != TB_STREAM_OK)
            {
                result = TB_ZIP_MALFORMED;
            }
            if (count == 0)
            {
                break;
            }
        }
        if (stream.functions != NULL)
        {
            (void)tb_stream_close(&stream);
        }
    }
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
    return result;
}

static size_t central_offset(const copy *input, size_t index)
{
    size_t at = 0;
    for (size_t found = 0; found <= index; found++)
    {
        at = find_bytes(input, found == 0 ? 0 : at + 4, "PK\x01\x02", 4);
    }
    return at;
}

static size_t end_offset(const copy *input)
{
    return find_bytes(input, 0, "PK\x05\x06", 4);
}

TB_TEST(codec_zip, every_truncation_is_refused)
{
    static copy input;
    copy_of(&input, "python.zip");
    for (size_t cut = 0; cut < input.length; cut++)
    {
        TB_TEST_CONTEXT("cut at %zu", cut);
        TB_TEST_EXPECT_UINT(open_and_read(input.bytes, cut, NULL), !=, TB_ZIP_OK);
    }
    /* Bytes after the end record, or before the archive, are refused too. */
    input.bytes[input.length] = 0;
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length + 1, NULL), ==, TB_ZIP_MALFORMED);
    memmove(input.bytes + 1, input.bytes, input.length);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length + 1, NULL), ==, TB_ZIP_MALFORMED);
}

TB_TEST(codec_zip, inconsistent_directories_are_refused)
{
    static copy input;
    size_t at = 0;
    /* The central directory's offset outside the archive. */
    copy_of(&input, "python.zip");
    put32(input.bytes + end_offset(&input) + 16, 0x7FFFFFF0u);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A size larger than the bytes before the end record. */
    copy_of(&input, "python.zip");
    put32(input.bytes + end_offset(&input) + 12, 0x7FFFFFF0u);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* More entries than records, and fewer. */
    copy_of(&input, "python.zip");
    at = end_offset(&input);
    put16(input.bytes + at + 8, 10);
    put16(input.bytes + at + 10, 10);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    put16(input.bytes + at + 8, 8);
    put16(input.bytes + at + 10, 8);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* Two entries at one offset. */
    copy_of(&input, "python.zip");
    put32(input.bytes + central_offset(&input, 1) + 42, 0);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* An entry that runs into the next. */
    copy_of(&input, "python.zip");
    put32(input.bytes + central_offset(&input, 0) + 20, 100);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A stored entry whose sizes differ. */
    copy_of(&input, "python.zip");
    put32(input.bytes + central_offset(&input, 4) + 24, 513);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A deflated entry claiming more than DEFLATE can expand to. */
    copy_of(&input, "python.zip");
    put32(input.bytes + central_offset(&input, 0) + 24, 14u * 1032u + 1025u);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A local header whose sizes disagree with the central record. */
    copy_of(&input, "python.zip");
    put32(input.bytes + 22, 13);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A local header whose name differs. */
    copy_of(&input, "python.zip");
    input.bytes[30] = 'j';
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A NUL in a name. */
    copy_of(&input, "python.zip");
    input.bytes[central_offset(&input, 0) + 46 + 2] = '\0';
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* An extra field longer than the room for it. */
    copy_of(&input, "python-extra.zip");
    put16(input.bytes + central_offset(&input, 0) + 46 + 9 + 2, 200);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A ZIP64 field that is missing. */
    copy_of(&input, "python.zip");
    put32(input.bytes + central_offset(&input, 0) + 24, 0xFFFFFFFFu);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
}

TB_TEST(codec_zip, damaged_data_is_malformed_when_read)
{
    static copy input;
    size_t at = 0;
    /* A CRC-32 that does not match. */
    copy_of(&input, "bsdtar.zip");
    at = central_offset(&input, 0);
    put32(input.bytes + at + 16, 0x12345678u);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A deflate stream that is not one. */
    copy_of(&input, "python.zip");
    at = find_bytes(&input, 0, "big.txt", 7) + 7;
    memset(input.bytes + at, 0xFF, 64);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    /* A size larger than the data holds, and smaller. */
    copy_of(&input, "bsdtar.zip");
    at = central_offset(&input, 0);
    put32(input.bytes + at + 24, 13);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
    put32(input.bytes + at + 24, 11);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_MALFORMED);
}

TB_TEST(codec_zip, what_cannot_be_read_here_is_unsupported)
{
    static copy input;
    size_t at = 0;
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    tb_stream stream = {0};
    tb_zip_entry entry = {0};
    /* Encryption. */
    copy_of(&input, "python.zip");
    at = central_offset(&input, 0);
    input.bytes[at + 8] |= 1u;
    TB_TEST_REQUIRE_UINT(open_bytes(input.bytes, input.length, NULL, &source, &archive), ==,
                         TB_ZIP_OK);
    tb_zip_entry_at(archive, 0, &entry);
    TB_TEST_EXPECT(entry.encrypted);
    TB_TEST_EXPECT_UINT(tb_zip_open_entry(archive, 0, NULL, &stream), ==, TB_ZIP_UNSUPPORTED);
    TB_TEST_EXPECT_POINTER(stream.functions, ==, NULL);
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
    /* Another method. */
    copy_of(&input, "python.zip");
    put16(input.bytes + central_offset(&input, 0) + 10, 12);
    TB_TEST_REQUIRE_UINT(open_bytes(input.bytes, input.length, NULL, &source, &archive), ==,
                         TB_ZIP_OK);
    TB_TEST_EXPECT_UINT(tb_zip_open_entry(archive, 0, NULL, &stream), ==, TB_ZIP_UNSUPPORTED);
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
    /* Several volumes. */
    copy_of(&input, "python.zip");
    put16(input.bytes + end_offset(&input) + 4, 1);
    TB_TEST_EXPECT_UINT(open_and_read(input.bytes, input.length, NULL), ==, TB_ZIP_UNSUPPORTED);
    /* A source that cannot read at an offset. */
    {
        static const tb_stream_functions sequential = {.release = nothing_to_release};
        tb_stream plain = {&sequential, NULL};
        TB_TEST_EXPECT_UINT(tb_zip_open(&plain, NULL, NULL, &archive), ==, TB_ZIP_UNSUPPORTED);
        TB_TEST_EXPECT_POINTER(archive, ==, NULL);
    }
}

TB_TEST(codec_zip, duplicate_names_are_refused_unless_allowed)
{
    static copy input;
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    size_t index = 0;
    const tb_zip_options allow = {.flags = TB_ZIP_ALLOW_DUPLICATE_NAMES};
    /* "empty.txt", in its local header and its central record, becomes "hello.txt". */
    copy_of(&input, "python.zip");
    TB_TEST_REQUIRE_UINT(replace_all(&input, "empty.txt", "hello.txt", 9), ==, 2);
    TB_TEST_EXPECT_UINT(open_bytes(input.bytes, input.length, NULL, &source, &archive), ==,
                        TB_ZIP_DUPLICATE_NAME);
    (void)tb_stream_close(&source);
    TB_TEST_REQUIRE_UINT(open_bytes(input.bytes, input.length, &allow, &source, &archive), ==,
                         TB_ZIP_OK);
    TB_TEST_REQUIRE(tb_zip_find(archive, "hello.txt", 9, &index));
    TB_TEST_EXPECT_UINT(index, ==, 1);
    tb_zip_close(archive);
    (void)tb_stream_close(&source);
}

/* Names that climb out, are absolute, or use backslashes are reported as
 * they are, and a rooted join refuses each one. */
TB_TEST(codec_zip, hostile_names_are_reported_as_they_are_and_refused_by_a_rooted_join)
{
    static const char *const names[] = {
        "../escape", "a/../../x", "/etc/pass", "C:/Win/x", "C:\\Win\\x", "a\\..\\..\\x",
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        static unsigned char bytes[4096];
        static char joined[256];
        const size_t length = tb_str_length(names[index]);
        char placeholder[32] = {0};
        sink state = {0};
        tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
        tb_zip_writer *const writer = tb_zip_writer_create(&stream, NULL);
        tb_zip_entry entry = {0};
        tb_stream source = {0};
        tb_zip_archive *archive = NULL;
        copy patched = {0};
        size_t required = 0;
        TB_TEST_CONTEXT("%s", names[index]);
        memset(placeholder, 'q', length);
        entry = entry_of(placeholder, TB_ZIP_FILE, TB_ZIP_STORED, 0);
        TB_TEST_REQUIRE_UINT(tb_zip_write_entry(writer, &entry, 0), ==, TB_ZIP_OK);
        TB_TEST_REQUIRE_UINT(tb_zip_writer_finish(writer), ==, TB_ZIP_OK);
        tb_zip_writer_destroy(writer);
        memcpy(patched.bytes, bytes, state.length);
        patched.length = state.length;
        memcpy(patched.bytes + 30, names[index], length);
        memcpy(patched.bytes + central_offset(&patched, 0) + 46, names[index], length);
        TB_TEST_REQUIRE_UINT(open_bytes(patched.bytes, patched.length, NULL, &source, &archive), ==,
                             TB_ZIP_OK);
        tb_zip_entry_at(archive, 0, &entry);
        TB_TEST_EXPECT_STRING(entry.name, ==, names[index]);
        TB_TEST_EXPECT_UINT(tb_path_rooted_join(TB_PATH_WINDOWS, (tb_path_view){"C:/out", 6},
                                                (tb_path_view){entry.name, entry.name_length},
                                                joined, sizeof(joined), &required),
                            !=, TB_PATH_OK);
        tb_zip_close(archive);
        (void)tb_stream_close(&source);
    }
}

/* Random damage to real archives is survived: whatever is read, it is read
 * within bounds, and reading ends. */
TB_TEST(codec_zip, mutated_archives_are_survived)
{
    static copy input;
    static const char *const archives[] = {"python.zip", "bsdtar.zip", "dotnet.zip",
                                           "python-zip64.zip", "python-extra.zip"};
    for (size_t trial = 0; trial < 1500; trial++)
    {
        const char *const name = archives[tb_test_random_below(TB_TEST_COUNT_OF(archives))];
        const size_t changes = (size_t)tb_test_random_between(1, 4);
        size_t length = 0;
        tb_zip_result result = TB_ZIP_OK;
        TB_TEST_CONTEXT("trial %zu of %s", trial, name);
        size_t directory = 0;
        copy_of(&input, name);
        length = input.length;
        directory = central_offset(&input, 0);
        for (size_t change = 0; change < changes; change++)
        {
            /* Most damage lands in the central directory and end record, where it matters. */
            const size_t at = tb_test_random_below(3) == 0
                                  ? (size_t)tb_test_random_below(length)
                                  : directory + (size_t)tb_test_random_below(length - directory);
            input.bytes[at] = (unsigned char)tb_test_random_below(256);
        }
        result = open_and_read(input.bytes, length, NULL);
        TB_TEST_EXPECT(result != TB_ZIP_INVALID && result != TB_ZIP_OUTPUT_FAILED &&
                       result != TB_ZIP_INPUT_FAILED);
    }
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void zip_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static unsigned char memory[512 * 1024];
    static unsigned char output[4096];
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_allocator allocator = tb_arena_allocator(arena);
    const tb_test_file *const file = find_file("python.zip");
    tb_stream source = {0};
    sink state = {0};
    tb_stream stream = open_sink(&state, output, sizeof(output), SIZE_MAX, SIZE_MAX);
    tb_zip_archive *archive = NULL;
    tb_zip_writer *writer = NULL;
    tb_zip_entry entry = entry_of("file", TB_ZIP_FILE, TB_ZIP_STORED, 2);
    tb_stream data = {0};
    tb_stream_open_memory(file->bytes, file->length, &allocator, &source);
    switch (choice)
    {
    case 0:
        (void)tb_zip_open(NULL, NULL, &allocator, &archive);
        break;
    case 1:
        (void)tb_zip_open(&source, &(tb_zip_options){.flags = 2}, &allocator, &archive);
        break;
    case 2:
        (void)tb_zip_open(&source, NULL, &allocator, &archive);
        tb_zip_entry_at(archive, tb_zip_count(archive), &entry);
        break;
    case 3:
        (void)tb_zip_open(&source, NULL, &allocator, &archive);
        (void)tb_zip_open_entry(archive, tb_zip_count(archive), &allocator, &data);
        break;
    case 4:
        writer = tb_zip_writer_create(NULL, &allocator);
        break;
    case 5:
        writer = tb_zip_writer_create(&stream, &allocator);
        (void)tb_zip_write_entry(writer, &entry, 0);
        entry.name = "other";
        entry.name_length = 5;
        (void)tb_zip_write_entry(writer, &entry, 0);
        break;
    case 6:
        writer = tb_zip_writer_create(&stream, &allocator);
        (void)tb_zip_write_entry(writer, &entry, 0);
        (void)tb_zip_write_data(writer, "abc", 3);
        break;
    case 7:
        writer = tb_zip_writer_create(&stream, &allocator);
        (void)tb_zip_write_entry(writer, &entry, 0);
        (void)tb_zip_writer_finish(writer);
        break;
    case 8:
        writer = tb_zip_writer_create(&stream, &allocator);
        (void)tb_zip_writer_finish(writer);
        (void)tb_zip_writer_finish(writer);
        break;
    default:
        writer = tb_zip_writer_create(&stream, &allocator);
        (void)tb_zip_writer_finish(writer);
        (void)tb_zip_write_entry(writer, &entry, 0);
        break;
    }
    tb_zip_close(archive);
    tb_zip_writer_destroy(writer);
    (void)tb_stream_close(&source);
    tb_arena_destroy(arena);
}

TB_TEST(codec_zip, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 10; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(zip_contract, &choice, "assertion failed");
    }
}
