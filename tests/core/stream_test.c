/* Streams: the operations on any stream, the helpers that loop over short
 * transfers, and the streams over memory, a growing buffer, files, and ranges.
 * A "trickle" stream that moves one byte at a time and fails on demand shows
 * that the helpers handle every short transfer and every failure. */
#include "badger/assert.h"
#include "badger/fs.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/thread.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

static tb_path_view path_view(const char *text)
{
    tb_path_view view = {0};
    view.data = text;
    view.length = tb_str_length(text);
    return view;
}

static void child_name(char *output, size_t capacity, const char *parent, const char *child)
{
    const size_t left = tb_str_length(parent);
    const size_t right = tb_str_length(child);
    TB_TEST_REQUIRE(left + right + 2 <= capacity);
    memcpy(output, parent, left);
    output[left] = '/';
    memcpy(output + left + 1, child, right + 1);
}

/* -------------------------------------------------------------------------
 * A stream for testing: reads from and writes to a fixed array one byte at a
 * time, and fails with a chosen result once a number of bytes has moved.
 * ------------------------------------------------------------------------- */

typedef struct trickle
{
    unsigned char bytes[64];
    size_t length; /* Bytes a read can return. */
    size_t position;
    size_t moved;      /* Bytes read or written so far. */
    size_t fail_after; /* Fail once this many bytes have moved. */
    tb_stream_result failure;
    size_t closes;
} trickle;

static tb_stream_result trickle_read(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    trickle *const source = state;
    (void)capacity;
    if (source->moved >= source->fail_after)
    {
        return source->failure;
    }
    if (source->position >= source->length)
    {
        return TB_STREAM_OK;
    }
    *(unsigned char *)bytes = source->bytes[source->position];
    source->position++;
    source->moved++;
    *transferred = 1;
    return TB_STREAM_OK;
}

static tb_stream_result trickle_write(void *state, const void *bytes, size_t length,
                                      size_t *transferred)
{
    trickle *const sink = state;
    (void)length;
    if (sink->moved >= sink->fail_after)
    {
        return sink->failure;
    }
    if (sink->position >= sizeof(sink->bytes))
    {
        return TB_STREAM_NO_SPACE;
    }
    sink->bytes[sink->position] = *(const unsigned char *)bytes;
    sink->position++;
    sink->moved++;
    if (sink->position > sink->length)
    {
        sink->length = sink->position;
    }
    *transferred = 1;
    return TB_STREAM_OK;
}

static tb_stream_result trickle_read_at(void *state, uint64_t offset, void *bytes, size_t capacity,
                                        size_t *transferred)
{
    trickle *const source = state;
    (void)capacity;
    if (source->moved >= source->fail_after)
    {
        return source->failure;
    }
    if (offset >= source->length)
    {
        return TB_STREAM_OK;
    }
    *(unsigned char *)bytes = source->bytes[(size_t)offset];
    source->moved++;
    *transferred = 1;
    return TB_STREAM_OK;
}

static tb_stream_result trickle_close(void *state)
{
    trickle *const stream = state;
    stream->closes++;
    return TB_STREAM_OK;
}

static const tb_stream_functions trickle_functions = {
    .read_bytes = trickle_read,
    .write_bytes = trickle_write,
    .read_bytes_at = trickle_read_at,
    .release = trickle_close,
};

static tb_stream trickle_open(trickle *state, const char *text)
{
    tb_stream stream = {0};
    *state = (trickle){0};
    state->length = tb_str_length(text);
    memcpy(state->bytes, text, state->length);
    state->fail_after = SIZE_MAX;
    stream.functions = &trickle_functions;
    stream.state = state;
    return stream;
}

/* -------------------------------------------------------------------------
 * The operations on any stream.
 * ------------------------------------------------------------------------- */

static tb_stream_result close_only(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

/* A stream offers what it can do; asking for anything else is a result, and a
 * flush of a stream that keeps nothing back succeeds. */
TB_TEST(core_stream, an_operation_the_stream_lacks_is_unsupported)
{
    static const tb_stream_functions functions = {.release = close_only};
    tb_stream stream = {&functions, NULL};
    unsigned char byte = {0};
    size_t count = 99;
    uint64_t value = 77;
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, &byte, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    count = 99;
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, &byte, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, 0, TB_STREAM_FROM_START, &value), ==,
                        TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &value), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_size(&stream, &value), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(value, ==, 77);
    count = 99;
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 0, &byte, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_flush(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, &byte, 1), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, &byte, 1), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_POINTER(stream.functions, ==, NULL);
    /* Closing a closed stream is accepted, and closes nothing. */
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
}

/* Zero-length transfers succeed without reaching the implementation, which
 * then never sees a request it could not make progress on. */
TB_TEST(core_stream, zero_length_transfers_do_not_reach_the_stream)
{
    trickle state = {0};
    tb_stream stream = trickle_open(&state, "abc");
    size_t count = 99;
    state.fail_after = 0;
    state.failure = TB_STREAM_OS_ERROR;
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, NULL, 0, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, NULL, 0, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 5, NULL, 0, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, NULL, 0), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, 0, NULL, 0), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, NULL, 0), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(state.closes, ==, 1);
}

/* The helpers loop over one-byte transfers, report the end of a stream that
 * is too short, and pass on the first failure as it was reported. */
TB_TEST(core_stream, helpers_loop_over_short_transfers_and_pass_failures_on)
{
    trickle state = {0};
    tb_stream stream = {0};
    unsigned char output[16] = {0};

    stream = trickle_open(&state, "streams");
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 7), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "streams", 7);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 1), ==, TB_STREAM_END);

    stream = trickle_open(&state, "short");
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 6), ==, TB_STREAM_END);
    /* What was read before the end is in the output. */
    TB_TEST_EXPECT_BYTES(output, "short", 5);

    stream = trickle_open(&state, "failing");
    state.fail_after = 3;
    state.failure = TB_STREAM_MALFORMED;
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 7), ==, TB_STREAM_MALFORMED);
    TB_TEST_EXPECT_BYTES(output, "fai", 3);

    stream = trickle_open(&state, "offsets");
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, 2, output, 5), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "fsets", 5);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, 2, output, 6), ==, TB_STREAM_END);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, UINT64_MAX, output, 2), ==,
                        TB_STREAM_TOO_LARGE);
    state.fail_after = state.moved + 1;
    state.failure = TB_STREAM_ACCESS;
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, 0, output, 3), ==, TB_STREAM_ACCESS);

    stream = trickle_open(&state, "");
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, "written", 7), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(state.length, ==, 7);
    TB_TEST_EXPECT_BYTES(state.bytes, "written", 7);
    state.fail_after = state.moved + 2;
    state.failure = TB_STREAM_NO_SPACE;
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, "more", 4), ==, TB_STREAM_NO_SPACE);
    TB_TEST_EXPECT_UINT(state.length, ==, 9);
}

/* A copy moves every byte through a buffer of any size, stops a source that
 * has more than the limit, and reports how far it got when a side fails. */
TB_TEST(core_stream, copy_respects_its_limit_and_reports_failures)
{
    static const char text[] = "a source of bytes";
    const size_t length = sizeof(text) - 1;
    trickle source_state = {0};
    trickle sink_state = {0};
    tb_stream source = {0};
    tb_stream sink = {0};
    unsigned char buffer[3] = {0};
    uint64_t copied = {0};
    const size_t capacities[] = {1, 2, 3};

    for (size_t row = {0}; row < TB_TEST_COUNT_OF(capacities); row++)
    {
        TB_TEST_CONTEXT("buffer of %zu", capacities[row]);
        source = trickle_open(&source_state, text);
        sink = trickle_open(&sink_state, "");
        TB_TEST_EXPECT_UINT(
            tb_stream_copy(&source, &sink, buffer, capacities[row], UINT64_MAX, &copied), ==,
            TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(copied, ==, length);
        TB_TEST_EXPECT_UINT(sink_state.length, ==, length);
        TB_TEST_EXPECT_BYTES(sink_state.bytes, text, length);
    }

    /* A limit of exactly the length is enough. */
    source = trickle_open(&source_state, text);
    sink = trickle_open(&sink_state, "");
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), length, &copied), ==,
                        TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(copied, ==, length);

    /* One byte less is not, and the limit is what was copied. */
    source = trickle_open(&source_state, text);
    sink = trickle_open(&sink_state, "");
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), length - 1, &copied),
                        ==, TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_UINT(copied, ==, length - 1);
    TB_TEST_EXPECT_UINT(sink_state.length, ==, length - 1);

    /* A limit of zero accepts only an empty source. */
    source = trickle_open(&source_state, "");
    sink = trickle_open(&sink_state, "");
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), 0, NULL), ==,
                        TB_STREAM_OK);
    source = trickle_open(&source_state, "x");
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), 0, &copied), ==,
                        TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_UINT(copied, ==, 0);

    source = trickle_open(&source_state, text);
    sink = trickle_open(&sink_state, "");
    source_state.fail_after = 5;
    source_state.failure = TB_STREAM_MALFORMED;
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), UINT64_MAX, &copied),
                        ==, TB_STREAM_MALFORMED);
    TB_TEST_EXPECT_UINT(copied, ==, 5);

    /* A source that fills the buffer, so a block can fail part of the way. */
    source = (tb_stream){0};
    tb_stream_open_memory(text, length, NULL, &source);
    sink = trickle_open(&sink_state, "");
    sink_state.fail_after = 4;
    sink_state.failure = TB_STREAM_NO_SPACE;
    TB_TEST_EXPECT_UINT(tb_stream_copy(&source, &sink, buffer, sizeof(buffer), UINT64_MAX, &copied),
                        ==, TB_STREAM_NO_SPACE);
    /* The block that failed part of the way is not counted. */
    TB_TEST_EXPECT_UINT(copied, ==, 3);
    TB_TEST_EXPECT_UINT(sink_state.length, ==, 4);
    TB_TEST_EXPECT_UINT(tb_stream_close(&source), ==, TB_STREAM_OK);
}

/* -------------------------------------------------------------------------
 * Seeking, which every built-in stream shares.
 * ------------------------------------------------------------------------- */

/* Positions run from zero to INT64_MAX. A seek outside them fails and leaves
 * the position where it was. */
TB_TEST(core_stream, seeks_stay_between_zero_and_int64_max)
{
    static const unsigned char bytes[10] = {0};
    static const struct
    {
        int64_t offset;
        tb_stream_origin origin;
        tb_stream_result result;
        uint64_t position;
    } rows[] = {
        {0, TB_STREAM_FROM_START, TB_STREAM_OK, 0},
        {7, TB_STREAM_FROM_START, TB_STREAM_OK, 7},
        {-3, TB_STREAM_FROM_END, TB_STREAM_OK, 7},
        {0, TB_STREAM_FROM_END, TB_STREAM_OK, 10},
        {25, TB_STREAM_FROM_END, TB_STREAM_OK, 35},
        {-11, TB_STREAM_FROM_END, TB_STREAM_INVALID, 4},
        {-5, TB_STREAM_FROM_CURRENT, TB_STREAM_INVALID, 4},
        {-4, TB_STREAM_FROM_CURRENT, TB_STREAM_OK, 0},
        {-1, TB_STREAM_FROM_START, TB_STREAM_INVALID, 4},
        {INT64_MIN, TB_STREAM_FROM_END, TB_STREAM_INVALID, 4},
        {INT64_MAX, TB_STREAM_FROM_START, TB_STREAM_OK, (uint64_t)INT64_MAX},
        {INT64_MAX, TB_STREAM_FROM_CURRENT, TB_STREAM_TOO_LARGE, 4},
        {INT64_MAX - 9, TB_STREAM_FROM_END, TB_STREAM_TOO_LARGE, 4},
        {INT64_MAX - 10, TB_STREAM_FROM_END, TB_STREAM_OK, (uint64_t)INT64_MAX},
        {0, (tb_stream_origin)99, TB_STREAM_INVALID, 4},
    };
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_stream stream = {0};
        uint64_t position = 1234;
        uint64_t after = {0};
        TB_TEST_CONTEXT("row %zu", row);
        tb_stream_open_memory(bytes, sizeof(bytes), NULL, &stream);
        TB_TEST_REQUIRE_UINT(tb_stream_seek(&stream, 4, TB_STREAM_FROM_START, NULL), ==,
                             TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, rows[row].offset, rows[row].origin, &position),
                            ==, rows[row].result);
        if (rows[row].result == TB_STREAM_OK)
        {
            TB_TEST_EXPECT_UINT(position, ==, rows[row].position);
        }
        else
        {
            TB_TEST_EXPECT_UINT(position, ==, 1234);
        }
        TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &after), ==, TB_STREAM_OK);
        TB_TEST_EXPECT_UINT(after, ==, rows[row].position);
        TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    }
}

/* -------------------------------------------------------------------------
 * Memory and the growing buffer.
 * ------------------------------------------------------------------------- */

TB_TEST(core_stream, memory_reads_seeks_and_reads_at_offsets_without_writing)
{
    static const char text[] = "0123456789";
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_stream stream = {0};
    unsigned char output[16] = {0};
    size_t count = {0};
    uint64_t value = {0};
    tb_stream_open_memory(text, 10, &allocator, &stream);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    TB_TEST_EXPECT_UINT(tb_stream_size(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 10);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 4, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 4);
    TB_TEST_EXPECT_BYTES(output, "0123", 4);
    /* A read at an offset leaves the position alone. */
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 8, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    TB_TEST_EXPECT_BYTES(output, "89", 2);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 10, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, UINT64_MAX, output, 16, &count), ==,
                        TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 4);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 6);
    TB_TEST_EXPECT_BYTES(output, "456789", 6);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    /* Past the end a read finds the end, as it does at the end. */
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, 100, TB_STREAM_FROM_START, NULL), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, "x", 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);

    /* No bytes at all is a stream that is always at its end. */
    tb_stream_open_memory(NULL, 0, &allocator, &stream);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 16, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 0);
}

TB_TEST(core_stream, a_buffer_grows_fills_gaps_with_zeros_and_reads_back)
{
    static const unsigned char zeros[5] = {0};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_stream stream = {0};
    unsigned char output[32] = {0};
    const unsigned char *bytes = NULL;
    size_t length = 99;
    size_t count = {0};
    uint64_t value = {0};
    tb_stream_open_buffer(&allocator, &stream);
    bytes = tb_stream_buffer_bytes(&stream, &length);
    TB_TEST_EXPECT_POINTER(bytes, ==, NULL);
    TB_TEST_EXPECT_UINT(length, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, "hello", 5), ==, TB_STREAM_OK);
    /* A seek past the end and a write leave zeros between. */
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, 5, TB_STREAM_FROM_END, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 10);
    TB_TEST_EXPECT_UINT(tb_stream_size(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 5);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, "world", 5), ==, TB_STREAM_OK);
    /* An overwrite in the middle does not change the size. */
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, 1, TB_STREAM_FROM_START, NULL), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, "EL", 2, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    bytes = tb_stream_buffer_bytes(&stream, &length);
    TB_TEST_REQUIRE_UINT(length, ==, 15);
    TB_TEST_EXPECT_BYTES(bytes, "hELlo", 5);
    TB_TEST_EXPECT_BYTES(bytes + 5, zeros, 5);
    TB_TEST_EXPECT_BYTES(bytes + 10, "world", 5);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 2, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "lo", 2);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 10, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 5);
    TB_TEST_EXPECT_UINT(tb_stream_flush(&stream), ==, TB_STREAM_OK);

    /* A write that needs memory there is not returns NO_MEMORY and changes nothing. */
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, 0, TB_STREAM_FROM_END, NULL), ==, TB_STREAM_OK);
    tb_test_allocator_fail_after(&tracker, 0);
    {
        unsigned char large[4096] = {0};
        TB_TEST_EXPECT_UINT(tb_stream_write(&stream, large, sizeof(large), &count), ==,
                            TB_STREAM_NO_MEMORY);
        TB_TEST_EXPECT_UINT(count, ==, 0);
    }
    tb_test_allocator_stop_failing(&tracker);
    bytes = tb_stream_buffer_bytes(&stream, &length);
    TB_TEST_EXPECT_UINT(length, ==, 15);
    TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 15);

    /* A position past what memory can address is too large to write at. */
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, INT64_MAX, TB_STREAM_FROM_START, NULL), ==,
                        TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, "x", 1, &count), ==, TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 0);
}

/* Random seeks, writes, and reads on a buffer agree with a plain array. */
TB_TEST(core_stream, a_buffer_agrees_with_a_simple_model)
{
    enum
    {
        MODEL_SIZE = 512,
        STEPS = 4000
    };
    unsigned char model[MODEL_SIZE] = {0};
    unsigned char scratch[64] = {0};
    unsigned char output[64] = {0};
    size_t model_length = {0};
    size_t model_position = {0};
    tb_stream stream = {0};
    tb_stream_open_buffer(NULL, &stream);
    for (size_t step = {0}; step < STEPS; step++)
    {
        const uint64_t choice = tb_test_random_below(3);
        size_t amount = (size_t)tb_test_random_below(sizeof(scratch) + 1);
        size_t count = {0};
        /* The model is a fixed array; writes stop at its end. */
        if (amount > MODEL_SIZE - model_position)
        {
            amount = MODEL_SIZE - model_position;
        }
        TB_TEST_CONTEXT("step %zu", step);
        if (choice == 0)
        {
            const size_t target = (size_t)tb_test_random_below(MODEL_SIZE - sizeof(scratch));
            uint64_t position = {0};
            TB_TEST_REQUIRE_UINT(
                tb_stream_seek(&stream, (int64_t)target, TB_STREAM_FROM_START, &position), ==,
                TB_STREAM_OK);
            TB_TEST_REQUIRE_UINT(position, ==, target);
            model_position = target;
        }
        else if (choice == 1)
        {
            tb_test_random_bytes(scratch, amount);
            TB_TEST_REQUIRE_UINT(tb_stream_write_all(&stream, scratch, amount), ==, TB_STREAM_OK);
            memcpy(model + model_position, scratch, amount);
            if (model_position + amount > model_length)
            {
                model_length = model_position + amount;
            }
            model_position += amount;
        }
        else
        {
            size_t expected = {0};
            if (model_position < model_length)
            {
                expected = model_length - model_position;
            }
            if (expected > amount)
            {
                expected = amount;
            }
            TB_TEST_REQUIRE_UINT(tb_stream_read(&stream, output, amount, &count), ==, TB_STREAM_OK);
            TB_TEST_REQUIRE_UINT(count, ==, expected);
            TB_TEST_REQUIRE_BYTES(output, model + model_position, count);
            model_position += count;
        }
    }
    {
        size_t length = {0};
        const unsigned char *const bytes = tb_stream_buffer_bytes(&stream, &length);
        TB_TEST_REQUIRE_UINT(length, ==, model_length);
        TB_TEST_EXPECT_BYTES(bytes, model, length);
    }
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);
}

/* -------------------------------------------------------------------------
 * Ranges.
 * ------------------------------------------------------------------------- */

TB_TEST(core_stream, a_range_is_a_window_of_its_parent_and_nests)
{
    static const char text[] = "0123456789abcdef";
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_stream parent = {0};
    tb_stream range = {0};
    tb_stream inner = {0};
    unsigned char output[32] = {0};
    size_t count = {0};
    uint64_t value = {0};
    tb_stream_open_memory(text, 16, &allocator, &parent);
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&parent, 4, 8, &allocator, &range), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_size(&range, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 8);
    TB_TEST_EXPECT_UINT(tb_stream_read(&range, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 8);
    TB_TEST_EXPECT_BYTES(output, "456789ab", 8);
    TB_TEST_EXPECT_UINT(tb_stream_read(&range, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_seek(&range, -2, TB_STREAM_FROM_END, NULL), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&range, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "ab", 2);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&range, 7, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 1);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&range, 8, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_write(&range, "x", 1, &count), ==, TB_STREAM_UNSUPPORTED);

    /* A range of a range is a window of the window. */
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&range, 2, 3, &allocator, &inner), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&inner, output, 3), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "678", 3);
    TB_TEST_EXPECT_UINT(tb_stream_close(&inner), ==, TB_STREAM_OK);

    /* A window larger than its parent ends where the parent does. */
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&range, 6, 100, &allocator, &inner), ==,
                         TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&inner, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 2);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&inner, output, 1), ==, TB_STREAM_END);
    TB_TEST_EXPECT_UINT(tb_stream_close(&inner), ==, TB_STREAM_OK);

    /* An empty window, and windows that end past INT64_MAX. */
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&parent, 16, 0, &allocator, &inner), ==,
                         TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&inner, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_close(&inner), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_open_range(&parent, (uint64_t)INT64_MAX, 1, &allocator, &inner),
                        ==, TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_POINTER(inner.functions, ==, NULL);
    TB_TEST_EXPECT_UINT(
        tb_stream_open_range(&parent, (uint64_t)INT64_MAX + 1, 0, &allocator, &inner), ==,
        TB_STREAM_TOO_LARGE);
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&parent, 0, (uint64_t)INT64_MAX, &allocator, &inner),
                         ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&inner), ==, TB_STREAM_OK);

    TB_TEST_EXPECT_UINT(tb_stream_close(&range), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&parent), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 0);
}

/* A range reads through its parent's reads at an offset, so a parent without
 * them cannot have one; and the range passes on its parent's failures. */
TB_TEST(core_stream, a_range_needs_reads_at_an_offset_and_passes_failures_on)
{
    static const tb_stream_functions sequential = {.read_bytes = trickle_read,
                                                   .release = trickle_close};
    trickle state = {0};
    tb_stream parent = trickle_open(&state, "parent");
    tb_stream range = {0};
    unsigned char output[8] = {0};
    size_t count = {0};
    parent.functions = &sequential;
    TB_TEST_EXPECT_UINT(tb_stream_open_range(&parent, 0, 4, NULL, &range), ==,
                        TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_POINTER(range.functions, ==, NULL);
    parent = trickle_open(&state, "parent");
    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&parent, 1, 4, NULL, &range), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&range, output, 4), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "aren", 4);
    state.fail_after = state.moved;
    state.failure = TB_STREAM_OS_ERROR;
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&range, 0, output, 4, &count), ==, TB_STREAM_OS_ERROR);
    TB_TEST_EXPECT_UINT(tb_stream_close(&range), ==, TB_STREAM_OK);
    /* Closing a range leaves its parent open. */
    TB_TEST_EXPECT_UINT(state.closes, ==, 0);
}

/* -------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

TB_TEST(core_stream, a_file_stream_keeps_a_position_of_its_own)
{
    char directory[32768] = {0};
    char path[32768] = {0};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_stream stream = {0};
    tb_stream other = {0};
    unsigned char output[32] = {0};
    size_t count = {0};
    uint64_t value = {0};
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, directory, sizeof(directory)), ==,
                         TB_FS_OK);
    child_name(path, sizeof(path), directory, "stream.bin");

    TB_TEST_EXPECT_UINT(tb_stream_open_file(path_view(path), TB_FILE_READ, &allocator, &stream), ==,
                        TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_POINTER(stream.functions, ==, NULL);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);

    TB_TEST_REQUIRE_UINT(tb_stream_open_file(path_view(path),
                                             TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE,
                                             &allocator, &stream),
                         ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&stream, "file stream", 11), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 11);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 5, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 6);
    TB_TEST_EXPECT_BYTES(output, "stream", 6);
    /* The read at an offset did not move the stream. */
    TB_TEST_EXPECT_UINT(tb_stream_tell(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 11);
    TB_TEST_EXPECT_UINT(tb_stream_seek(&stream, -6, TB_STREAM_FROM_END, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 5);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 6), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "stream", 6);
    TB_TEST_EXPECT_UINT(tb_stream_read(&stream, output, 32, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stream_size(&stream, &value), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(value, ==, 11);
    TB_TEST_EXPECT_UINT(tb_stream_flush(&stream), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);

    /* What a file stream offers follows from its flags. */
    TB_TEST_REQUIRE_UINT(tb_stream_open_file(path_view(path), TB_FILE_READ, &allocator, &stream),
                         ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_stream_write(&stream, "x", 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 4), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "file", 4);
    TB_TEST_REQUIRE_UINT(tb_stream_open_file(path_view(path), TB_FILE_WRITE, &allocator, &other),
                         ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_stream_read(&other, output, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&other, 0, output, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_write_all(&other, "F", 1), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_flush(&other), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&other), ==, TB_STREAM_OK);
    /* Two streams over one file each keep their own position. */
    TB_TEST_EXPECT_UINT(tb_stream_read_exact_at(&stream, 0, output, 1), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, "F", 1);
    TB_TEST_EXPECT_UINT(tb_stream_read_exact(&stream, output, 7), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(output, " stream", 7);
    TB_TEST_EXPECT_UINT(tb_stream_close(&stream), ==, TB_STREAM_OK);

    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(directory)), ==, TB_FS_OK);
}

/* -------------------------------------------------------------------------
 * Reads at an offset from many threads.
 * ------------------------------------------------------------------------- */

enum
{
    SHARED_THREADS = 8,
    SHARED_SIZE = 64 * 1024,
    SHARED_READS = 1000
};

static unsigned char shared_byte(uint64_t offset)
{
    return (unsigned char)((offset * UINT64_C(2654435761)) >> 11);
}

typedef struct shared_reader
{
    tb_stream *stream;
    uint64_t base;   /* Where the stream's first byte is in the pattern. */
    uint64_t length; /* Bytes in the stream. */
    uint64_t seed;
    size_t failures;
} shared_reader;

static uint64_t shared_next(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void shared_read(void *argument)
{
    shared_reader *const reader = argument;
    unsigned char buffer[512] = {0};
    uint64_t state = reader->seed;
    for (size_t read = {0}; read < SHARED_READS; read++)
    {
        const uint64_t offset = shared_next(&state) % (reader->length + 16);
        const size_t capacity = 1 + (size_t)(shared_next(&state) % sizeof(buffer));
        size_t count = {0};
        if (tb_stream_read_at(reader->stream, offset, buffer, capacity, &count) != TB_STREAM_OK)
        {
            reader->failures++;
            continue;
        }
        for (size_t byte = {0}; byte < count; byte++)
        {
            if (buffer[byte] != shared_byte(reader->base + offset + byte))
            {
                reader->failures++;
                break;
            }
        }
    }
}

static void read_from_threads(tb_stream *stream, uint64_t base, uint64_t length)
{
    tb_thread threads[SHARED_THREADS] = {0};
    shared_reader readers[SHARED_THREADS] = {0};
    for (size_t index = {0}; index < SHARED_THREADS; index++)
    {
        readers[index].stream = stream;
        readers[index].base = base;
        readers[index].length = length;
        readers[index].seed = UINT64_C(0x9E3779B97F4A7C15) * (index + 1);
        tb_thread_create(&threads[index], shared_read, &readers[index]);
    }
    for (size_t index = {0}; index < SHARED_THREADS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(readers[index].failures, ==, 0);
    }
}

/* The claim of stream.h: reads at an offset from a memory, file, or range
 * stream run together from any number of threads. */
TB_TEST(core_stream, reads_at_an_offset_run_together_on_every_built_in_stream)
{
    static unsigned char pattern[SHARED_SIZE] = {0};
    char directory[32768] = {0};
    char path[32768] = {0};
    tb_stream memory = {0};
    tb_stream file = {0};
    tb_stream range = {0};
    for (size_t index = {0}; index < SHARED_SIZE; index++)
    {
        pattern[index] = shared_byte(index);
    }
    tb_stream_open_memory(pattern, sizeof(pattern), NULL, &memory);
    TB_TEST_CONTEXT("memory");
    read_from_threads(&memory, 0, SHARED_SIZE);

    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, directory, sizeof(directory)), ==,
                         TB_FS_OK);
    child_name(path, sizeof(path), directory, "shared.bin");
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(path_view(path), pattern, sizeof(pattern)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_open_file(path_view(path), TB_FILE_READ, NULL, &file), ==,
                         TB_FS_OK);
    TB_TEST_CONTEXT("file");
    read_from_threads(&file, 0, SHARED_SIZE);

    TB_TEST_REQUIRE_UINT(tb_stream_open_range(&file, 1000, 20000, NULL, &range), ==, TB_STREAM_OK);
    TB_TEST_CONTEXT("range of a file");
    read_from_threads(&range, 1000, 20000);

    TB_TEST_EXPECT_UINT(tb_stream_close(&range), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&file), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&memory), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(path_view(directory)), ==, TB_FS_OK);
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static tb_stream_result overreporting_read(void *state, void *bytes, size_t capacity,
                                           size_t *transferred)
{
    (void)state;
    (void)bytes;
    *transferred = capacity + 1;
    return TB_STREAM_OK;
}

static tb_stream_result silent_write(void *state, const void *bytes, size_t length,
                                     size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)length;
    *transferred = 0;
    return TB_STREAM_OK;
}

static const tb_stream_functions broken_functions = {
    .read_bytes = overreporting_read,
    .write_bytes = silent_write,
    .release = close_only,
};

static void broken_stream_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static const unsigned char bytes[4] = {0};
    unsigned char byte = {0};
    size_t count = {0};
    uint64_t value = {0};
    trickle state = {0};
    tb_stream closed = {0};
    tb_stream open = trickle_open(&state, "abcd");
    tb_stream other = {0};
    tb_stream broken = {&broken_functions, NULL};
    switch (choice)
    {
    case 0:
        (void)tb_stream_read(NULL, &byte, 1, &count);
        break;
    case 1:
        (void)tb_stream_read(&closed, &byte, 1, &count);
        break;
    case 2:
        (void)tb_stream_read(&open, &byte, 1, NULL);
        break;
    case 3:
        (void)tb_stream_read(&open, NULL, 1, &count);
        break;
    case 4:
        (void)tb_stream_write(&open, NULL, 1, &count);
        break;
    case 5:
        (void)tb_stream_tell(&open, NULL);
        break;
    case 6:
        (void)tb_stream_size(&open, NULL);
        break;
    case 7:
        (void)tb_stream_read_at(&open, 0, &byte, 1, NULL);
        break;
    case 8:
        (void)tb_stream_close(NULL);
        break;
    case 9:
        (void)tb_stream_read_exact(&open, NULL, 1);
        break;
    case 10:
        (void)tb_stream_write_all(&open, NULL, 1);
        break;
    case 11:
        (void)tb_stream_copy(&open, &open, &byte, 1, UINT64_MAX, NULL);
        break;
    case 12:
        (void)tb_stream_copy(&open, &other, &byte, 0, UINT64_MAX, NULL);
        break;
    case 13:
        (void)tb_stream_buffer_bytes(&open, &count);
        break;
    case 14:
        tb_stream_open_memory(bytes, sizeof(bytes), NULL, &open);
        break;
    case 15:
        tb_stream_open_buffer(NULL, &open);
        break;
    case 16:
        (void)tb_stream_open_range(&open, 0, 1, NULL, &open);
        break;
    case 17:
        (void)tb_stream_read(&broken, &byte, 1, &count);
        break;
    case 18:
        (void)tb_stream_write(&broken, &byte, 1, &count);
        break;
    case 19:
        tb_stream_open_memory(NULL, 1, NULL, &other);
        break;
    case 20:
        (void)tb_stream_seek(&closed, 0, TB_STREAM_FROM_START, &value);
        break;
    default:
        break;
    }
}

TB_TEST(core_stream, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 21; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(broken_stream_contract, &choice, "assertion failed");
    }
}
