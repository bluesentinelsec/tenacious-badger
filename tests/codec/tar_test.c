/* TAR reading and writing: archives that GNU tar, bsdtar, and Python's
 * tarfile wrote in ustar, pax, and GNU format; round trips of the writer's
 * own archives, at every limit of the ustar fields; streams of one byte at a
 * time and through gzip; and damaged and hostile archives, which must be
 * reported, never trusted. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/deflate.h"
#include "badger/fmt.h"
#include "badger/path.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/tar.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdbool.h>
#include <string.h>

extern const tb_test_file tb_tar_files[];
extern const size_t tb_tar_files_count;

#define BLOCK ((size_t)512)
#define MODIFIED INT64_C(1700000000)
#define OCTAL_11_MAX UINT64_C(077777777777)

/* -------------------------------------------------------------------------
 * The entries every reference archive holds, as tools/make_tar_test_data.py
 * writes them.
 * ------------------------------------------------------------------------- */

/* Entries some archives leave out. */
enum
{
    NEEDS_LONG_NAMES = 1, /* Longer than ustar can hold: not in ustar archives. */
    NEEDS_UTF8_NAMES = 2, /* Not in bsdtar's, which garbles them on Windows. */
    NEEDS_SYMLINKS = 4,   /* Not in GNU tar's, made from a file system without them. */
    NEEDS_HARD_LINKS = 8  /* Only in Python's and the writer's own. */
};

/* How an entry's data is made. */
typedef enum content
{
    CONTENT_TEXT,    /* `text`. */
    CONTENT_STRIDE,  /* 1000 bytes of (index * 7 + 3) & 0xFF. */
    CONTENT_COUNTING /* 512 bytes of index & 0xFF. */
} content;

typedef struct expected_entry
{
    const char *name;
    tb_tar_type type;
    uint32_t mode;
    const char *link;
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

static char name_100[101];
static char prefix_name[148];
static char long_name[309];

static const expected_entry expected_entries[] = {
    {"hello.txt", TB_TAR_FILE, 0644, "", CONTENT_TEXT, "Hello, tar!\n", 0},
    {"empty.txt", TB_TAR_FILE, 0644, "", CONTENT_TEXT, "", 0},
    {"dir", TB_TAR_DIRECTORY, 0755, "", CONTENT_TEXT, "", 0},
    {"dir/data.bin", TB_TAR_FILE, 0644, "", CONTENT_STRIDE, NULL, 0},
    {"exact.bin", TB_TAR_FILE, 0600, "", CONTENT_COUNTING, NULL, 0},
    {name_100, TB_TAR_FILE, 0644, "", CONTENT_TEXT, "one hundred\n", 0},
    {prefix_name, TB_TAR_FILE, 0644, "", CONTENT_TEXT, "prefix\n", 0},
    {long_name, TB_TAR_FILE, 0644, "", CONTENT_TEXT, "long\n", NEEDS_LONG_NAMES},
    {UTF8_NAME, TB_TAR_FILE, 0644, "", CONTENT_TEXT, UTF8_TEXT, NEEDS_UTF8_NAMES},
    {"run.sh", TB_TAR_FILE, 0755, "", CONTENT_TEXT, "#!/bin/sh\necho run\n", 0},
    {"link", TB_TAR_SYMLINK, 0777, "hello.txt", CONTENT_TEXT, "", NEEDS_SYMLINKS},
    {"hard", TB_TAR_HARDLINK, 0644, "hello.txt", CONTENT_TEXT, "", NEEDS_HARD_LINKS},
};

static void make_names(void)
{
    memset(name_100, 'n', 96);
    memcpy(name_100 + 96, ".txt", 5);
    memset(prefix_name, 'p', 120);
    memcpy(prefix_name + 120, "/needs-the-prefix-field.txt", 28);
    for (size_t part = 0; part < 3; part++)
    {
        memset(long_name + part * 100, 'd', 99);
        long_name[part * 100 + 99] = '/';
    }
    memcpy(long_name + 300, "long.txt", 9);
}

/* The entry's data, into `bytes` of room for 1000; returns its length. */
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
    case CONTENT_TEXT:
    default:
        length = tb_str_length(entry->text);
        memcpy(bytes, entry->text, length);
        break;
    }
    return length;
}

/* What a writer's archive leaves out, by its file name, such as "bsdtar-ustar.tar". */
static unsigned missing_from(const char *file)
{
    unsigned missing = 0;
    if (tb_str_find(file, 0, TB_STR_END, "-ustar") != TB_STR_NOT_FOUND)
    {
        missing |= NEEDS_LONG_NAMES;
    }
    if (tb_str_starts_with(file, "bsdtar"))
    {
        missing |= NEEDS_UTF8_NAMES | NEEDS_HARD_LINKS;
    }
    if (tb_str_starts_with(file, "gnutar"))
    {
        missing |= NEEDS_SYMLINKS | NEEDS_HARD_LINKS;
    }
    return missing;
}

static bool is_compressed(const tb_test_file *file)
{
    return tb_str_find(file->name, 0, TB_STR_END, ".gz") != TB_STR_NOT_FOUND;
}

static const tb_test_file *find_file(const char *name)
{
    const tb_test_file *found = NULL;
    for (size_t index = 0; index < tb_tar_files_count; index++)
    {
        if (tb_str_equal(tb_tar_files[index].name, name))
        {
            found = &tb_tar_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    return found;
}

/* Read the current entry's data whole, `chunk` bytes at a time. */
static size_t read_data(tb_tar_reader *reader, unsigned char *bytes, size_t capacity, size_t chunk)
{
    size_t length = 0;
    for (;;)
    {
        size_t count = 0;
        const size_t want = capacity - length < chunk ? capacity - length : chunk;
        TB_TEST_REQUIRE_UINT(tb_tar_reader_read(reader, bytes + length, want, &count), ==,
                             TB_TAR_OK);
        if (count == 0)
        {
            return length;
        }
        length += count;
    }
}

/* Read an archive and require exactly the expected entries but those in `missing`. */
static void expect_entries(tb_stream *source, unsigned missing, size_t chunk)
{
    tb_tar_reader *const reader = tb_tar_reader_create(source, NULL, NULL);
    tb_tar_entry entry = {0};
    static unsigned char data[1001];
    static unsigned char expected[1000];
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
    {
        const expected_entry *const want = &expected_entries[index];
        size_t length = 0;
        if ((want->needs & missing) != 0)
        {
            continue;
        }
        TB_TEST_CONTEXT("entry %zu", index);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_STRING(entry.name, ==, want->name);
        TB_TEST_EXPECT_UINT(entry.name_length, ==, tb_str_length(want->name));
        TB_TEST_EXPECT_UINT(entry.type, ==, want->type);
        TB_TEST_EXPECT_UINT(entry.mode, ==, want->mode);
        TB_TEST_EXPECT_INT(entry.modified, ==, MODIFIED);
        TB_TEST_EXPECT_STRING(entry.link, ==, want->link);
        length = content_of(want, expected);
        TB_TEST_EXPECT_UINT(entry.size, ==, length);
        /* Read only some of one entry; the next call skips the rest. */
        if (index == 3)
        {
            size_t count = 0;
            TB_TEST_REQUIRE_UINT(tb_tar_reader_read(reader, data, 10, &count), ==, TB_TAR_OK);
            TB_TEST_REQUIRE_UINT(count, >, 0);
            TB_TEST_EXPECT_BYTES(data, expected, count);
            continue;
        }
        TB_TEST_REQUIRE_UINT(read_data(reader, data, sizeof(data), chunk), ==, length);
        TB_TEST_EXPECT_BYTES(data, expected, length);
    }
    TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_END);
    TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_END);
    TB_TEST_EXPECT_POINTER(entry.name, ==, NULL);
    tb_tar_reader_destroy(reader);
}

/* -------------------------------------------------------------------------
 * Streams for the tests.
 * ------------------------------------------------------------------------- */

/* Reads borrowed bytes, at most `step` at a time. */
typedef struct trickle
{
    const unsigned char *bytes;
    size_t length;
    size_t position;
    size_t step;
} trickle;

static tb_stream_result trickle_read(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    trickle *const source = state;
    size_t count = source->length - source->position;
    if (count > capacity)
    {
        count = capacity;
    }
    if (count > source->step)
    {
        count = source->step;
    }
    memcpy(bytes, source->bytes + source->position, count);
    source->position += count;
    *transferred = count;
    return TB_STREAM_OK;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

static const tb_stream_functions trickle_functions = {
    .read_bytes = trickle_read,
    .release = nothing_to_release,
};

static tb_stream open_trickle(trickle *state, const void *bytes, size_t length, size_t step)
{
    const tb_stream stream = {&trickle_functions, state};
    *state = (trickle){.bytes = bytes, .length = length, .step = step};
    return stream;
}

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

/* -------------------------------------------------------------------------
 * Writing the expected entries.
 * ------------------------------------------------------------------------- */

static tb_tar_entry entry_of(const char *name, tb_tar_type type, uint64_t size)
{
    return (tb_tar_entry){
        .name = name,
        .name_length = tb_str_length(name),
        .link = "",
        .type = type,
        .size = size,
        .modified = MODIFIED,
    };
}

static void write_expected(tb_tar_writer *writer)
{
    static unsigned char data[1000];
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
    {
        const expected_entry *const want = &expected_entries[index];
        const size_t length = content_of(want, data);
        tb_tar_entry entry = entry_of(want->name, want->type, length);
        entry.link = want->link;
        entry.link_length = tb_str_length(want->link);
        entry.mode = want->mode;
        TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
        /* In pieces, to show that pieces make the same archive. */
        for (size_t at = 0; at < length; at += 300)
        {
            const size_t piece = length - at < 300 ? length - at : 300;
            TB_TEST_REQUIRE_UINT(tb_tar_write_data(writer, data + at, piece), ==, TB_TAR_OK);
        }
    }
    TB_TEST_REQUIRE_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
}

/* Write the expected entries into `bytes` through a sink of `step` bytes a write. */
static size_t write_expected_archive(unsigned char *bytes, size_t capacity, size_t step)
{
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, capacity, step, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    write_expected(writer);
    tb_tar_writer_destroy(writer);
    return state.length;
}

/* -------------------------------------------------------------------------
 * Reading what other tools wrote.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_tar, archives_of_three_writers_in_three_formats_read_exactly)
{
    size_t archives = 0;
    make_names();
    for (size_t index = 0; index < tb_tar_files_count; index++)
    {
        const tb_test_file *const file = &tb_tar_files[index];
        tb_stream source = {0};
        if (is_compressed(file))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        tb_stream_open_memory(file->bytes, file->length, NULL, &source);
        expect_entries(&source, missing_from(file->name), 1000);
        (void)tb_stream_close(&source);
        archives++;
    }
    TB_TEST_EXPECT_UINT(archives, ==, 9);
}

TB_TEST(codec_tar, reading_a_byte_at_a_time_reads_the_same)
{
    make_names();
    for (size_t index = 0; index < tb_tar_files_count; index++)
    {
        const tb_test_file *const file = &tb_tar_files[index];
        trickle state = {0};
        tb_stream source = {0};
        if (is_compressed(file))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        source = open_trickle(&state, file->bytes, file->length, 1);
        expect_entries(&source, missing_from(file->name), 1);
    }
}

TB_TEST(codec_tar, a_gzip_archive_reads_through_decompression)
{
    const tb_test_file *const file = find_file("gnutar-pax.tar.gz");
    tb_stream compressed = {0};
    tb_stream source = {0};
    make_names();
    tb_stream_open_memory(file->bytes, file->length, NULL, &compressed);
    TB_TEST_REQUIRE_UINT(tb_inflate_open_stream(&compressed, TB_DEFLATE_GZIP, NULL, &source), ==,
                         TB_STREAM_OK);
    expect_entries(&source, missing_from("gnutar-pax"), 1000);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&compressed);
}

TB_TEST(codec_tar, an_entry_reads_as_a_stream_until_the_reader_moves_on)
{
    const tb_test_file *const file = find_file("python-pax.tar");
    tb_stream source = {0};
    tb_stream data = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_entry entry = {0};
    unsigned char bytes[64] = {0};
    size_t count = 0;
    tb_stream_open_memory(file->bytes, file->length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    tb_tar_reader_open_data(reader, &data);
    TB_TEST_REQUIRE_UINT(tb_stream_read_exact(&data, bytes, 12), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(bytes, "Hello, tar!\n", 12);
    TB_TEST_EXPECT_UINT(tb_stream_read(&data, bytes, sizeof(bytes), &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&data, bytes, sizeof(bytes), &count), ==, TB_STREAM_INVALID);
    (void)tb_stream_close(&data);
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
}

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_tar, the_writer_writes_what_the_reader_reads)
{
    static unsigned char archive[1 << 15];
    static unsigned char again[1 << 15];
    size_t length = 0;
    tb_stream source = {0};
    make_names();
    length = write_expected_archive(archive, sizeof(archive), SIZE_MAX);
    TB_TEST_EXPECT_UINT(length % BLOCK, ==, 0);
    tb_stream_open_memory(archive, length, NULL, &source);
    expect_entries(&source, 0, 1000);
    (void)tb_stream_close(&source);
    /* The same entries make the same bytes, whatever the sink takes at a time. */
    TB_TEST_EXPECT_UINT(write_expected_archive(again, sizeof(again), 1), ==, length);
    TB_TEST_EXPECT_BYTES(again, archive, length);
}

TB_TEST(codec_tar, the_writer_archive_survives_gzip)
{
    static unsigned char archive[1 << 15];
    tb_stream buffer = {0};
    tb_stream compressor = {0};
    tb_stream compressed = {0};
    tb_stream source = {0};
    const unsigned char *bytes = NULL;
    size_t length = 0;
    tb_tar_writer *writer = NULL;
    make_names();
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_REQUIRE_UINT(tb_deflate_open_stream(&buffer, TB_DEFLATE_GZIP, 6, NULL, &compressor), ==,
                         TB_STREAM_OK);
    writer = tb_tar_writer_create(&compressor, NULL);
    write_expected(writer);
    tb_tar_writer_destroy(writer);
    TB_TEST_REQUIRE_UINT(tb_stream_close(&compressor), ==, TB_STREAM_OK);
    bytes = tb_stream_buffer_bytes(&buffer, &length);
    TB_TEST_EXPECT_UINT(length, <, write_expected_archive(archive, sizeof(archive), SIZE_MAX));
    tb_stream_open_memory(bytes, length, NULL, &compressed);
    TB_TEST_REQUIRE_UINT(tb_inflate_open_stream(&compressed, TB_DEFLATE_GZIP, NULL, &source), ==,
                         TB_STREAM_OK);
    expect_entries(&source, 0, 1000);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&compressed);
    (void)tb_stream_close(&buffer);
}

/* Write one entry; return the archive's length. */
static size_t write_one(const tb_tar_entry *entry, unsigned char *bytes, size_t capacity)
{
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, capacity, SIZE_MAX, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, entry), ==, TB_TAR_OK);
    TB_TEST_REQUIRE_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
    tb_tar_writer_destroy(writer);
    return state.length;
}

/* Read back an archive of one entry and require it to equal `entry`. */
static void expect_one(const unsigned char *bytes, size_t length, const tb_tar_entry *entry)
{
    tb_stream source = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_entry read = {0};
    tb_stream_open_memory(bytes, length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &read), ==, TB_TAR_OK);
    TB_TEST_EXPECT_UINT(read.name_length, ==, entry->name_length);
    TB_TEST_EXPECT_BYTES(read.name, entry->name, entry->name_length);
    TB_TEST_EXPECT_UINT(read.link_length, ==, entry->link_length);
    TB_TEST_EXPECT_BYTES(read.link, entry->link, entry->link_length);
    TB_TEST_EXPECT_UINT(read.type, ==, entry->type);
    TB_TEST_EXPECT_INT(read.modified, ==, entry->modified);
    TB_TEST_EXPECT_UINT(read.size, ==, entry->size);
    TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &read), ==, TB_TAR_END);
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
}

TB_TEST(codec_tar, names_and_links_at_every_limit_of_the_ustar_fields)
{
    static char name[512];
    static char link[200];
    static unsigned char archive[4 * 1024];
    /* Each name as runs: `prefix` bytes, then a slash and `rest` bytes if
     * `rest` is not zero; and whether it needs a pax header. */
    static const struct
    {
        size_t prefix;
        size_t rest;
        tb_tar_type type;
        bool pax;
    } names[] = {
        {1, 0, TB_TAR_FILE, false},         {99, 0, TB_TAR_FILE, false},
        {100, 0, TB_TAR_FILE, false},       {101, 0, TB_TAR_FILE, true},
        {99, 0, TB_TAR_DIRECTORY, false}, /* 100 with its slash. */
        {100, 0, TB_TAR_DIRECTORY, true}, /* 101 with its slash, nowhere to split. */
        {155, 100, TB_TAR_FILE, false},   /* The most the two fields hold: 256. */
        {156, 99, TB_TAR_FILE, true},     /* A prefix of 156. */
        {10, 101, TB_TAR_FILE, true},     /* A last component of 101. */
        {155, 99, TB_TAR_DIRECTORY, false}, {300, 99, TB_TAR_FILE, true},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        size_t length = names[index].prefix;
        size_t archive_length = 0;
        tb_tar_entry entry = {0};
        TB_TEST_CONTEXT("name %zu", index);
        memset(name, 'a', names[index].prefix);
        if (names[index].rest > 0)
        {
            name[length] = '/';
            memset(name + length + 1, 'b', names[index].rest);
            length += 1 + names[index].rest;
        }
        name[length] = '\0';
        entry = entry_of(name, names[index].type, 0);
        archive_length = write_one(&entry, archive, sizeof(archive));
        TB_TEST_EXPECT_UINT(archive[156], ==,
                            (unsigned)(names[index].pax
                                           ? 'x'
                                           : (names[index].type == TB_TAR_DIRECTORY ? '5' : '0')));
        expect_one(archive, archive_length, &entry);
    }
    for (size_t length = 99; length <= 101; length++)
    {
        tb_tar_entry entry = entry_of("link", TB_TAR_SYMLINK, 0);
        size_t archive_length = 0;
        TB_TEST_CONTEXT("link of %zu", length);
        memset(link, 'l', length);
        entry.link = link;
        entry.link_length = length;
        archive_length = write_one(&entry, archive, sizeof(archive));
        TB_TEST_EXPECT_UINT(archive[156], ==, (unsigned)(length > 100 ? 'x' : '2'));
        expect_one(archive, archive_length, &entry);
    }
}

TB_TEST(codec_tar, times_beyond_the_octal_fields_go_in_pax)
{
    static unsigned char archive[4 * 1024];
    static const int64_t times[] = {
        0, 1, MODIFIED, (int64_t)OCTAL_11_MAX, (int64_t)OCTAL_11_MAX + 1, -1, INT64_MIN, INT64_MAX,
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(times); index++)
    {
        tb_tar_entry entry = entry_of("time", TB_TAR_DIRECTORY, 0);
        const bool pax = times[index] < 0 || times[index] > (int64_t)OCTAL_11_MAX;
        size_t length = 0;
        TB_TEST_CONTEXT("time %zu", index);
        entry.modified = times[index];
        length = write_one(&entry, archive, sizeof(archive));
        TB_TEST_EXPECT_UINT(archive[156], ==, (unsigned)(pax ? 'x' : '5'));
        expect_one(archive, length, &entry);
    }
}

/* The header for a file of `size` bytes, without its data, as the writer writes it. */
static size_t write_size_header(uint64_t size, unsigned char *bytes, size_t capacity)
{
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, capacity, SIZE_MAX, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    const tb_tar_entry entry = entry_of("big", TB_TAR_FILE, size);
    TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
    /* Left unfinished, with all its data owed. */
    tb_tar_writer_destroy(writer);
    return state.length;
}

TB_TEST(codec_tar, sizes_at_each_side_of_the_octal_limit)
{
    static unsigned char archive[4 * 1024];
    static const uint64_t sizes[] = {OCTAL_11_MAX, OCTAL_11_MAX + 1, UINT64_C(1) << 40};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        const bool pax = sizes[index] > OCTAL_11_MAX;
        const size_t length = write_size_header(sizes[index], archive, sizeof(archive));
        tb_stream source = {0};
        tb_tar_reader *reader = NULL;
        tb_tar_entry entry = {0};
        TB_TEST_CONTEXT("size %zu", index);
        TB_TEST_EXPECT_UINT(length, ==, pax ? 3 * BLOCK : BLOCK);
        TB_TEST_EXPECT_UINT(archive[156], ==, (unsigned)(pax ? 'x' : '0'));
        if (!pax)
        {
            TB_TEST_EXPECT_BYTES(archive + 124, "77777777777", 12);
        }
        tb_stream_open_memory(archive, length, NULL, &source);
        reader = tb_tar_reader_create(&source, NULL, NULL);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_UINT(entry.size, ==, sizes[index]);
        /* The data is missing. */
        TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_TRUNCATED);
        tb_tar_reader_destroy(reader);
        (void)tb_stream_close(&source);
    }
}

TB_TEST(codec_tar, modes_default_by_type)
{
    static unsigned char archive[4 * 1024];
    static const struct
    {
        tb_tar_type type;
        uint32_t given;
        uint32_t written;
    } modes[] = {
        {TB_TAR_FILE, 0, 0644},     {TB_TAR_DIRECTORY, 0, 0755}, {TB_TAR_SYMLINK, 0, 0777},
        {TB_TAR_HARDLINK, 0, 0644}, {TB_TAR_FILE, 04755, 04755}, {TB_TAR_FILE, 07777, 07777},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(modes); index++)
    {
        tb_tar_entry entry = entry_of("mode", modes[index].type, 0);
        tb_stream source = {0};
        tb_tar_reader *reader = NULL;
        size_t length = 0;
        TB_TEST_CONTEXT("mode %zu", index);
        if (modes[index].type == TB_TAR_SYMLINK || modes[index].type == TB_TAR_HARDLINK)
        {
            entry.link = "target";
            entry.link_length = 6;
        }
        entry.mode = modes[index].given;
        length = write_one(&entry, archive, sizeof(archive));
        tb_stream_open_memory(archive, length, NULL, &source);
        reader = tb_tar_reader_create(&source, NULL, NULL);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_UINT(entry.mode, ==, modes[index].written);
        tb_tar_reader_destroy(reader);
        (void)tb_stream_close(&source);
    }
}

TB_TEST(codec_tar, many_small_entries_round_trip)
{
    enum
    {
        ENTRIES = 2000
    };
    static unsigned char archive[4 << 20];
    static unsigned char data[1500];
    sink state = {0};
    tb_stream stream = open_sink(&state, archive, sizeof(archive), SIZE_MAX, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    tb_tar_reader *reader = NULL;
    tb_stream source = {0};
    const uint64_t seed = tb_test_random_u64();
    uint64_t state_bits = seed;
    tb_tar_entry entry = {0};
    char name[32] = {0};
    for (size_t index = 0; index < ENTRIES; index++)
    {
        const size_t size = (size_t)(state_bits % sizeof(data));
        state_bits = state_bits * UINT64_C(6364136223846793005) + 1442695040888963407u;
        (void)tb_fmt_buffer(name, sizeof(name), "file-%05zu", index);
        entry = entry_of(name, TB_TAR_FILE, size);
        memset(data, (int)(index & 0xFFu), size);
        TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
        TB_TEST_REQUIRE_UINT(tb_tar_write_data(writer, data, size), ==, TB_TAR_OK);
    }
    TB_TEST_REQUIRE_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
    tb_tar_writer_destroy(writer);
    tb_stream_open_memory(archive, state.length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    state_bits = seed;
    for (size_t index = 0; index < ENTRIES; index++)
    {
        static unsigned char read[1500];
        const size_t size = (size_t)(state_bits % sizeof(data));
        state_bits = state_bits * UINT64_C(6364136223846793005) + 1442695040888963407u;
        TB_TEST_CONTEXT("entry %zu", index);
        (void)tb_fmt_buffer(name, sizeof(name), "file-%05zu", index);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_REQUIRE_STRING(entry.name, ==, name);
        TB_TEST_REQUIRE_UINT(read_data(reader, read, sizeof(read), sizeof(read)), ==, size);
        memset(data, (int)(index & 0xFFu), size);
        TB_TEST_REQUIRE_BYTES(read, data, size);
    }
    TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_END);
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
}

TB_TEST(codec_tar, entries_the_writer_cannot_write_are_refused_whole)
{
    static unsigned char archive[4 * 1024];
    static const char *const bad_names[] = {
        "", "/a", "a/", "a//b", ".", "..", "./a", "a/.", "a/../b", "../a", "a\xff", "a\xc3",
    };
    sink state = {0};
    tb_stream stream = open_sink(&state, archive, sizeof(archive), SIZE_MAX, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    tb_tar_entry entry = {0};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bad_names); index++)
    {
        TB_TEST_CONTEXT("name %zu", index);
        entry = entry_of(bad_names[index], TB_TAR_FILE, 0);
        TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    }
    entry = entry_of("a", TB_TAR_FILE, 0);
    entry.name = "a\0b";
    entry.name_length = 3;
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry = entry_of("a", TB_TAR_OTHER, 0);
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry = entry_of("a", TB_TAR_DIRECTORY, 1);
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry = entry_of("a", TB_TAR_SYMLINK, 0);
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry.link = "t\xff";
    entry.link_length = 2;
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry = entry_of("a", TB_TAR_FILE, 0);
    entry.link = "t";
    entry.link_length = 1;
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    entry = entry_of("a", TB_TAR_FILE, 0);
    entry.mode = 010000;
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_INVALID);
    /* Nothing was written, and the writer goes on. */
    TB_TEST_EXPECT_UINT(state.length, ==, 0);
    entry = entry_of("a", TB_TAR_FILE, 0);
    TB_TEST_EXPECT_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
    tb_tar_writer_destroy(writer);
}

TB_TEST(codec_tar, a_sink_that_refuses_is_reported_from_then_on)
{
    static unsigned char archive[1 << 15];
    static unsigned char out[1 << 15];
    size_t length = 0;
    make_names();
    length = write_expected_archive(archive, sizeof(archive), SIZE_MAX);
    for (size_t limit = 0; limit < length; limit += 211)
    {
        sink state = {0};
        tb_stream stream = open_sink(&state, out, sizeof(out), SIZE_MAX, limit);
        tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
        static unsigned char data[1000];
        bool failed = false;
        TB_TEST_CONTEXT("limit %zu", limit);
        for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
        {
            const expected_entry *const want = &expected_entries[index];
            const size_t size = content_of(want, data);
            tb_tar_entry entry = entry_of(want->name, want->type, size);
            tb_tar_result result = TB_TAR_OK;
            entry.link = want->link;
            entry.link_length = tb_str_length(want->link);
            entry.mode = want->mode;
            result = tb_tar_write_entry(writer, &entry);
            if (result == TB_TAR_OK)
            {
                result = tb_tar_write_data(writer, data, size);
            }
            else
            {
                /* The header failed; its data is still owed, and is refused too. */
                TB_TEST_EXPECT_UINT(tb_tar_write_data(writer, data, size), ==,
                                    TB_TAR_OUTPUT_FAILED);
            }
            TB_TEST_EXPECT(!failed || result == TB_TAR_OUTPUT_FAILED);
            failed = failed || result == TB_TAR_OUTPUT_FAILED;
        }
        TB_TEST_EXPECT_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OUTPUT_FAILED);
        TB_TEST_EXPECT_UINT(state.length, ==, limit);
        TB_TEST_EXPECT_BYTES(out, archive, limit);
        tb_tar_writer_destroy(writer);
    }
}

/* -------------------------------------------------------------------------
 * Damaged and hostile archives, made a block at a time.
 * ------------------------------------------------------------------------- */

typedef struct archive
{
    unsigned char bytes[16 * 1024];
    size_t length;
} archive;

static void put_octal(unsigned char *header, size_t offset, size_t width, uint64_t value)
{
    size_t index = width - 1;
    header[offset + index] = '\0';
    while (index > 0)
    {
        index--;
        header[offset + index] = (unsigned char)('0' + (value & 7u));
        value >>= 3;
    }
}

/* Recompute a header's checksum, as POSIX has it. */
static void seal(unsigned char *header)
{
    unsigned sum = 0;
    memset(header + 148, ' ', 8);
    for (size_t index = 0; index < BLOCK; index++)
    {
        sum += header[index];
    }
    put_octal(header, 148, 7, sum);
}

/* Append a ustar header, sealed, and return it for changes that need a new seal. */
static unsigned char *add_header(archive *output, const char *name, char flag, uint64_t size)
{
    unsigned char *const header = output->bytes + output->length;
    TB_TEST_REQUIRE_UINT(output->length + BLOCK, <=, sizeof(output->bytes));
    memset(header, 0, BLOCK);
    memcpy(header, name, tb_str_length(name));
    put_octal(header, 100, 8, 0644);
    put_octal(header, 108, 8, 0);
    put_octal(header, 116, 8, 0);
    put_octal(header, 124, 12, size);
    put_octal(header, 136, 12, (uint64_t)MODIFIED);
    header[156] = (unsigned char)flag;
    memcpy(header + 257,
           "ustar\0"
           "00",
           8);
    seal(header);
    output->length += BLOCK;
    return header;
}

/* Append data, padded to its block. */
static void add_data(archive *output, const void *bytes, size_t length)
{
    const size_t padded = (length + BLOCK - 1) / BLOCK * BLOCK;
    TB_TEST_REQUIRE_UINT(output->length + padded, <=, sizeof(output->bytes));
    memcpy(output->bytes + output->length, bytes, length);
    memset(output->bytes + output->length + length, 0, padded - length);
    output->length += padded;
}

static void add_pax(archive *output, const char *records)
{
    const size_t length = tb_str_length(records);
    (void)add_header(output, "pax", 'x', length);
    add_data(output, records, length);
}

static void add_end(archive *output)
{
    static const unsigned char zeros[2 * 512] = {0};
    add_data(output, zeros, sizeof(zeros));
}

/* Read every entry and its data; return how reading ended. */
static tb_tar_result read_all(const unsigned char *bytes, size_t length,
                              const tb_tar_options *options, uint64_t *offset)
{
    tb_stream source = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_result result = TB_TAR_OK;
    tb_stream_open_memory(bytes, length, NULL, &source);
    reader = tb_tar_reader_create(&source, options, NULL);
    for (;;)
    {
        tb_tar_entry entry = {0};
        static unsigned char data[4096];
        size_t count = 0;
        result = tb_tar_reader_next(reader, &entry);
        if (result != TB_TAR_OK)
        {
            break;
        }
        do
        {
            result = tb_tar_reader_read(reader, data, sizeof(data), &count);
        } while (result == TB_TAR_OK && count > 0);
        if (result != TB_TAR_OK)
        {
            break;
        }
    }
    if (offset != NULL)
    {
        *offset = tb_tar_reader_offset(reader);
    }
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
    return result;
}

static tb_tar_result read_archive(const archive *input, uint64_t *offset)
{
    return read_all(input->bytes, input->length, NULL, offset);
}

TB_TEST(codec_tar, every_truncation_is_reported)
{
    static unsigned char bytes[1 << 15];
    static size_t boundaries[64];
    size_t boundary_count = 0;
    size_t length = 0;
    const tb_tar_options allow = {.flags = TB_TAR_ALLOW_MISSING_END};
    sink state = {0};
    tb_stream stream = open_sink(&state, bytes, sizeof(bytes), SIZE_MAX, SIZE_MAX);
    tb_tar_writer *const writer = tb_tar_writer_create(&stream, NULL);
    make_names();
    /* The expected entries, noting where each begins. */
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_entries); index++)
    {
        static unsigned char data[1000];
        const expected_entry *const want = &expected_entries[index];
        const size_t size = content_of(want, data);
        tb_tar_entry entry = entry_of(want->name, want->type, size);
        entry.link = want->link;
        entry.link_length = tb_str_length(want->link);
        boundaries[boundary_count] = state.length;
        boundary_count++;
        TB_TEST_REQUIRE_UINT(tb_tar_write_entry(writer, &entry), ==, TB_TAR_OK);
        TB_TEST_REQUIRE_UINT(tb_tar_write_data(writer, data, size), ==, TB_TAR_OK);
    }
    boundaries[boundary_count] = state.length;
    boundary_count++;
    TB_TEST_REQUIRE_UINT(tb_tar_writer_finish(writer), ==, TB_TAR_OK);
    tb_tar_writer_destroy(writer);
    length = state.length;
    for (size_t cut = 0; cut <= length; cut++)
    {
        /* The first zero block is enough to end, as some writers leave only
         * one; a second cut short is a block cut short. */
        const bool ended = cut == length - BLOCK || cut == length;
        const size_t within = cut % BLOCK;
        bool at_boundary = false;
        for (size_t index = 0; index < boundary_count; index++)
        {
            at_boundary = at_boundary || boundaries[index] == cut;
        }
        if (cut > 3 * BLOCK && within > 1 && within < BLOCK - 1 && within != BLOCK / 2)
        {
            continue;
        }
        TB_TEST_CONTEXT("cut at %zu of %zu", cut, length);
        TB_TEST_EXPECT_UINT(read_all(bytes, cut, NULL, NULL), ==,
                            ended ? TB_TAR_END : TB_TAR_TRUNCATED);
        TB_TEST_EXPECT_UINT(read_all(bytes, cut, &allow, NULL), ==,
                            ended || at_boundary ? TB_TAR_END : TB_TAR_TRUNCATED);
    }
}

TB_TEST(codec_tar, damaged_headers_are_malformed_where_they_are)
{
    static const struct
    {
        size_t at; /* A byte of the second header to change. */
        unsigned char to;
        bool reseal;
    } damage[] = {
        {0, 'X', false},   /* A checksum that does not match. */
        {150, '9', false}, /* A checksum that is not octal. */
        {101, '8', true},  /* A mode that is not octal. */
        {125, 'x', true},  /* A size that is not octal. */
        {130, ' ', true},  /* A space inside a size. */
        {137, '-', true},  /* A time that is not octal. */
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(damage); index++)
    {
        static archive input;
        unsigned char *header = NULL;
        uint64_t offset = 0;
        TB_TEST_CONTEXT("damage %zu", index);
        input.length = 0;
        (void)add_header(&input, "first", '0', 3);
        add_data(&input, "abc", 3);
        header = add_header(&input, "second", '0', 0);
        header[damage[index].at] = damage[index].to;
        if (damage[index].reseal)
        {
            seal(header);
        }
        add_end(&input);
        TB_TEST_EXPECT_UINT(read_archive(&input, &offset), ==, TB_TAR_MALFORMED);
        TB_TEST_EXPECT_UINT(offset, ==, 2 * BLOCK);
    }
}

TB_TEST(codec_tar, malformed_extended_headers_are_refused)
{
    static const struct
    {
        const char *records;
        tb_tar_result result;
    } records[] = {
        {"6 a=b\n", TB_TAR_OK},
        {"12 comment=\n", TB_TAR_OK},
        {"7 a=b\n", TB_TAR_MALFORMED},  /* The length runs past the end. */
        {"5 a=b\n", TB_TAR_MALFORMED},  /* The length ends before the newline. */
        {"6 ab\n\n", TB_TAR_MALFORMED}, /* No '='. */
        {"6 =ab\n", TB_TAR_MALFORMED},  /* No key. */
        {"x a=b\n", TB_TAR_MALFORMED},  /* No length. */
        {"6a=bc\n", TB_TAR_MALFORMED},  /* No space after the length. */
        {"0 \n", TB_TAR_MALFORMED},
        {"99999999999999999999999 a=b\n", TB_TAR_MALFORMED},
        {"14 size=12x45\n", TB_TAR_MALFORMED},
        {"8 size=\n", TB_TAR_MALFORMED},
        {"18 size=-12345678\n", TB_TAR_MALFORMED},
        {"29 size=18446744073709551616\n", TB_TAR_MALFORMED},
        {"29 size=18446744073709551615\n", TB_TAR_MALFORMED}, /* No room for its padding. */
        {"12 mtime=1x\n", TB_TAR_MALFORMED},
        {"13 mtime=1.x\n", TB_TAR_MALFORMED},
        {"29 mtime=9223372036854775808\n", TB_TAR_MALFORMED},
        {"22 GNU.sparse.major=1\n", TB_TAR_UNSUPPORTED},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(records); index++)
    {
        static archive input;
        TB_TEST_CONTEXT("%s", records[index].records);
        input.length = 0;
        add_pax(&input, records[index].records);
        (void)add_header(&input, "entry", '0', 0);
        add_end(&input);
        TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==,
                            records[index].result == TB_TAR_OK ? TB_TAR_END
                                                               : records[index].result);
    }
}

TB_TEST(codec_tar, extended_values_reach_their_entry)
{
    static const struct
    {
        const char *records;
        int64_t modified;
    } times[] = {
        {"20 mtime=1700000000\n", MODIFIED},
        {"24 mtime=1700000000.999\n", MODIFIED},
        {"12 mtime=-1\n", -1},
        {"14 mtime=-1.5\n", -2},
        {"14 mtime=-1.0\n", -1},
        {"14 mtime=-0.5\n", -1},
        {"30 mtime=-9223372036854775808\n", INT64_MIN},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(times); index++)
    {
        static archive input;
        tb_stream source = {0};
        tb_tar_reader *reader = NULL;
        tb_tar_entry entry = {0};
        TB_TEST_CONTEXT("%s", times[index].records);
        input.length = 0;
        add_pax(&input, times[index].records);
        (void)add_header(&input, "entry", '0', 0);
        add_end(&input);
        tb_stream_open_memory(input.bytes, input.length, NULL, &source);
        reader = tb_tar_reader_create(&source, NULL, NULL);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_INT(entry.modified, ==, times[index].modified);
        tb_tar_reader_destroy(reader);
        (void)tb_stream_close(&source);
    }
}

/* A header's numeric field in GNU's base-256. */
static void put_base256(unsigned char *header, size_t offset, size_t width, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    for (size_t index = width; index > 0; index--)
    {
        header[offset + index - 1] = (unsigned char)(bits & 0xFFu);
        bits = value < 0 ? (bits >> 8) | (UINT64_C(0xFF) << 56) : bits >> 8;
    }
    header[offset] = value < 0 ? 0xFFu : 0x80u;
}

TB_TEST(codec_tar, gnu_base256_numbers_read_exactly)
{
    static archive input;
    unsigned char *header = NULL;
    tb_stream source = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_entry entry = {0};
    input.length = 0;
    header = add_header(&input, "big", '0', 0);
    put_base256(header, 124, 12, INT64_C(1) << 36);
    put_base256(header, 136, 12, -INT64_C(1000));
    seal(header);
    tb_stream_open_memory(input.bytes, input.length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_UINT(entry.size, ==, UINT64_C(1) << 36);
    TB_TEST_EXPECT_INT(entry.modified, ==, -1000);
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
    /* More than 64 bits. */
    input.length = 0;
    header = add_header(&input, "big", '0', 0);
    memset(header + 124, 0xFF, 12);
    header[124] = 0x80;
    seal(header);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
}

TB_TEST(codec_tar, structural_faults_are_reported)
{
    static archive input;
    tb_tar_options small = {.max_name = 10, .max_extended = 100};
    unsigned char *header = NULL;
    /* A zero block, then a header. */
    input.length = 0;
    add_data(&input, (const unsigned char[512]){0}, BLOCK);
    (void)add_header(&input, "late", '0', 0);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    /* An extended header, then the end. */
    input.length = 0;
    add_pax(&input, "6 a=b\n");
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    /* An extended header, then nothing, which no option allows. */
    input.length = 0;
    add_pax(&input, "6 a=b\n");
    TB_TEST_EXPECT_UINT(read_all(input.bytes, input.length,
                                 &(tb_tar_options){.flags = TB_TAR_ALLOW_MISSING_END}, NULL),
                        ==, TB_TAR_TRUNCATED);
    /* Data longer than the archive. */
    input.length = 0;
    (void)add_header(&input, "short", '0', 2000);
    add_data(&input, "abc", 3);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_TRUNCATED);
    /* Sparse files and continuations from another volume. */
    input.length = 0;
    (void)add_header(&input, "sparse", 'S', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_UNSUPPORTED);
    input.length = 0;
    (void)add_header(&input, "continued", 'M', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_UNSUPPORTED);
    /* An empty name, a link without a target, and an empty GNU long name. */
    input.length = 0;
    (void)add_header(&input, "", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    input.length = 0;
    (void)add_header(&input, "/", '5', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    input.length = 0;
    (void)add_header(&input, "link", '2', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    input.length = 0;
    (void)add_header(&input, "././@LongLink", 'L', 1);
    add_data(&input, "", 1);
    (void)add_header(&input, "x", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    /* Limits on names and extended headers. */
    input.length = 0;
    (void)add_header(&input, "eleven-byte", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_all(input.bytes, input.length, &small, NULL), ==, TB_TAR_TOO_LARGE);
    input.length = 0;
    add_pax(&input, "17 path=12345678\n");
    (void)add_header(&input, "x", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_all(input.bytes, input.length, &small, NULL), ==, TB_TAR_END);
    input.length = 0;
    add_pax(&input, "20 path=12345678901\n");
    (void)add_header(&input, "x", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_all(input.bytes, input.length, &small, NULL), ==, TB_TAR_TOO_LARGE);
    input.length = 0;
    header = add_header(&input, "pax", 'x', 101);
    (void)header;
    add_data(&input, (const unsigned char[101]){0}, 101);
    TB_TEST_EXPECT_UINT(read_all(input.bytes, input.length, &small, NULL), ==, TB_TAR_TOO_LARGE);
    /* A NUL in a pax path. */
    input.length = 0;
    (void)add_header(&input, "pax", 'x', 9);
    add_data(&input, "9 path=\0\n", 9);
    (void)add_header(&input, "x", '0', 0);
    add_end(&input);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
    /* A block that is not a header at all. */
    input.length = 0;
    add_data(&input, "This is not an archive.", 23);
    TB_TEST_EXPECT_UINT(read_archive(&input, NULL), ==, TB_TAR_MALFORMED);
}

TB_TEST(codec_tar, older_and_gnu_headers_read_their_own_way)
{
    static archive input;
    unsigned char *header = NULL;
    tb_stream source = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_entry entry = {0};
    input.length = 0;
    /* No magic: the original format, where a slash at the end marks a directory. */
    header = add_header(&input, "old/", '\0', 0);
    memset(header + 257, 0, 8);
    seal(header);
    /* GNU's magic: the prefix field holds other things, and is not part of the name. */
    header = add_header(&input, "gnu", '0', 0);
    memcpy(header + 257, "ustar  \0", 8);
    memset(header + 345, 'z', 12);
    seal(header);
    /* A checksum of signed bytes, as some old writers made it. */
    header = add_header(&input, "signed\xe9", '0', 0);
    {
        int sum = 0;
        memset(header + 148, ' ', 8);
        for (size_t index = 0; index < BLOCK; index++)
        {
            sum += (signed char)header[index];
        }
        put_octal(header, 148, 7, (uint64_t)sum);
    }
    /* A pax global header, whose records are checked and then ignored. */
    (void)add_header(&input, "global", 'g', 16);
    add_data(&input, "16 path=ignored\n", 16);
    (void)add_header(&input, "last", '7', 0);
    add_end(&input);
    tb_stream_open_memory(input.bytes, input.length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_STRING(entry.name, ==, "old");
    TB_TEST_EXPECT_UINT(entry.type, ==, TB_TAR_DIRECTORY);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_STRING(entry.name, ==, "gnu");
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_STRING(entry.name, ==, "signed\xe9");
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_STRING(entry.name, ==, "last");
    TB_TEST_EXPECT_UINT(entry.type, ==, TB_TAR_FILE);
    TB_TEST_EXPECT_INT(entry.flag, ==, '7');
    TB_TEST_EXPECT_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_END);
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
    /* Devices and other things are reported, with their flag. */
    input.length = 0;
    (void)add_header(&input, "fifo", '6', 0);
    (void)add_header(&input, "label", 'V', 0);
    add_end(&input);
    tb_stream_open_memory(input.bytes, input.length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_UINT(entry.type, ==, TB_TAR_OTHER);
    TB_TEST_EXPECT_INT(entry.flag, ==, '6');
    TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
    TB_TEST_EXPECT_INT(entry.flag, ==, 'V');
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
}

/* Names and link targets that climb out, are absolute, or are not UTF-8 are
 * reported exactly as they are, and a rooted join refuses each one. */
TB_TEST(codec_tar, hostile_names_are_reported_as_they_are_and_refused_by_a_rooted_join)
{
    static const struct
    {
        const char *name;
        bool safe;
    } names[] = {
        {"../escape", false},
        {"a/../../escape", false},
        {"/etc/passwd", false},
        {"C:/Windows/evil", false},
        {"C:\\Windows\\evil", false},
        {"\\\\server\\share\\evil", false},
        {"a\\..\\..\\escape", false},
        {"bad\xff"
         "name",
         false},
        {"a/./b", true},
        {"plain/name.txt", true},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        static archive input;
        tb_stream source = {0};
        tb_tar_reader *reader = NULL;
        tb_tar_entry entry = {0};
        static char joined[512];
        size_t required = 0;
        tb_path_result joined_result = TB_PATH_OK;
        TB_TEST_CONTEXT("%s", names[index].name);
        input.length = 0;
        (void)add_header(&input, names[index].name, '0', 0);
        (void)add_header(&input, "link", '2', 0);
        memcpy(input.bytes + BLOCK + 157, names[index].name, tb_str_length(names[index].name));
        seal(input.bytes + BLOCK);
        add_end(&input);
        tb_stream_open_memory(input.bytes, input.length, NULL, &source);
        reader = tb_tar_reader_create(&source, NULL, NULL);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_STRING(entry.name, ==, names[index].name);
        joined_result = tb_path_rooted_join(TB_PATH_WINDOWS, (tb_path_view){"C:/out", 6},
                                            (tb_path_view){entry.name, entry.name_length}, joined,
                                            sizeof(joined), &required);
        TB_TEST_EXPECT((joined_result == TB_PATH_OK) == names[index].safe);
        TB_TEST_REQUIRE_UINT(tb_tar_reader_next(reader, &entry), ==, TB_TAR_OK);
        TB_TEST_EXPECT_STRING(entry.link, ==, names[index].name);
        tb_tar_reader_destroy(reader);
        (void)tb_stream_close(&source);
    }
}

/* Random damage to real archives is survived: whatever is read, it is
 * read within bounds, and reading ends. */
TB_TEST(codec_tar, mutated_archives_are_survived)
{
    static unsigned char bytes[1 << 15];
    for (size_t trial = 0; trial < 1500; trial++)
    {
        const tb_test_file *const file = &tb_tar_files[tb_test_random_below(tb_tar_files_count)];
        const size_t changes = (size_t)tb_test_random_between(1, 6);
        size_t length = file->length;
        tb_tar_result result = TB_TAR_OK;
        if (is_compressed(file))
        {
            continue;
        }
        TB_TEST_CONTEXT("trial %zu of %s", trial, file->name);
        TB_TEST_REQUIRE_UINT(length, <=, sizeof(bytes));
        memcpy(bytes, file->bytes, length);
        for (size_t change = 0; change < changes; change++)
        {
            /* Most damage lands in headers, where it matters. */
            const size_t block = (size_t)tb_test_random_below(length / BLOCK);
            const size_t at = block * BLOCK + (size_t)tb_test_random_below(
                                                  tb_test_random_below(2) == 0 ? 160 : BLOCK);
            bytes[at] = (unsigned char)tb_test_random_below(256);
        }
        if (tb_test_random_below(4) == 0)
        {
            length = (size_t)tb_test_random_below(length + 1);
        }
        result = read_all(bytes, length, NULL, NULL);
        TB_TEST_EXPECT(result != TB_TAR_OK && result != TB_TAR_INVALID &&
                       result != TB_TAR_OUTPUT_FAILED && result != TB_TAR_INPUT_FAILED);
    }
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void tar_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static unsigned char memory[16 * 1024];
    static unsigned char output[4096];
    static const unsigned char empty[1024] = {0};
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_stream source = {0};
    sink state = {0};
    tb_stream stream = open_sink(&state, output, sizeof(output), SIZE_MAX, SIZE_MAX);
    tb_tar_reader *reader = NULL;
    tb_tar_writer *writer = NULL;
    tb_tar_entry entry = entry_of("file", TB_TAR_FILE, 2);
    unsigned char byte = 0;
    size_t count = 0;
    tb_stream data = {0};
    tb_stream_open_memory(empty, sizeof(empty), &allocator, &source);
    switch (choice)
    {
    case 0:
        reader = tb_tar_reader_create(NULL, NULL, &allocator);
        break;
    case 1:
        reader = tb_tar_reader_create(&source, &(tb_tar_options){.flags = 2}, &allocator);
        break;
    case 2:
        reader = tb_tar_reader_create(&source, NULL, &allocator);
        (void)tb_tar_reader_read(reader, &byte, 1, &count);
        break;
    case 3:
        reader = tb_tar_reader_create(&source, NULL, &allocator);
        (void)tb_tar_reader_next(reader, &entry);
        (void)tb_tar_reader_read(reader, &byte, 1, &count);
        break;
    case 4:
        reader = tb_tar_reader_create(&source, NULL, &allocator);
        tb_tar_reader_open_data(reader, &data);
        break;
    case 5:
        writer = tb_tar_writer_create(NULL, &allocator);
        break;
    case 6:
        writer = tb_tar_writer_create(&stream, &allocator);
        (void)tb_tar_write_entry(writer, &entry);
        (void)tb_tar_write_entry(writer, &entry);
        break;
    case 7:
        writer = tb_tar_writer_create(&stream, &allocator);
        (void)tb_tar_write_entry(writer, &entry);
        (void)tb_tar_write_data(writer, "abc", 3);
        break;
    case 8:
        writer = tb_tar_writer_create(&stream, &allocator);
        (void)tb_tar_write_entry(writer, &entry);
        (void)tb_tar_writer_finish(writer);
        break;
    case 9:
        writer = tb_tar_writer_create(&stream, &allocator);
        (void)tb_tar_writer_finish(writer);
        (void)tb_tar_writer_finish(writer);
        break;
    default:
        writer = tb_tar_writer_create(&stream, &allocator);
        (void)tb_tar_writer_finish(writer);
        (void)tb_tar_write_entry(writer, &entry);
        break;
    }
    tb_tar_reader_destroy(reader);
    tb_tar_writer_destroy(writer);
    (void)tb_stream_close(&source);
    tb_arena_destroy(arena);
}

TB_TEST(codec_tar, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 11; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(tar_contract, &choice, "assertion failed");
    }
}
