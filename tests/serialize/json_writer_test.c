/* JSON writing: the exact text of every kind of value in both styles, the
 * escapes of every byte, numbers that read back exactly, values JSON cannot
 * hold refused, a sink that fails, and structure misuse caught. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/json.h"
#include "badger/str.h"
#include "tb_test.h"

#include <string.h>

/* A writer into a buffer stream, and what it wrote as a C string. */
typedef struct written
{
    tb_stream sink;
    tb_json_writer *writer;
    char text[8192];
    size_t length;
} written;

static void start(written *output, tb_json_style style)
{
    *output = (written){0};
    tb_stream_open_buffer(NULL, &output->sink);
    output->writer = tb_json_writer_create(&output->sink, style, NULL);
}

static void finish(written *output)
{
    size_t length = 0;
    const unsigned char *bytes = NULL;
    TB_TEST_REQUIRE_UINT(tb_json_writer_finish(output->writer), ==, TB_JSON_OK);
    bytes = tb_stream_buffer_bytes(&output->sink, &length);
    TB_TEST_REQUIRE_UINT(length, <, sizeof(output->text));
    if (length > 0)
    {
        memcpy(output->text, bytes, length);
    }
    output->text[length] = '\0';
    output->length = length;
    tb_json_writer_destroy(output->writer);
    output->writer = NULL;
    (void)tb_stream_close(&output->sink);
}

/* The same calls in both styles. */
static void write_sample(tb_json_writer *writer)
{
    (void)tb_json_write_begin_object(writer);
    (void)tb_json_write_key(writer, "name", 4);
    (void)tb_json_write_string(writer, "rock", 4);
    (void)tb_json_write_key(writer, "sizes", 5);
    (void)tb_json_write_begin_array(writer);
    (void)tb_json_write_int64(writer, -1);
    (void)tb_json_write_uint64(writer, 2);
    (void)tb_json_write_double(writer, 2.5);
    (void)tb_json_write_end_array(writer);
    (void)tb_json_write_key(writer, "empty", 5);
    (void)tb_json_write_begin_array(writer);
    (void)tb_json_write_end_array(writer);
    (void)tb_json_write_key(writer, "none", 4);
    (void)tb_json_write_begin_object(writer);
    (void)tb_json_write_end_object(writer);
    (void)tb_json_write_key(writer, "flags", 5);
    (void)tb_json_write_begin_array(writer);
    (void)tb_json_write_bool(writer, true);
    (void)tb_json_write_bool(writer, false);
    (void)tb_json_write_null(writer);
    (void)tb_json_write_end_array(writer);
    (void)tb_json_write_end_object(writer);
}

TB_TEST(serialize_json_writer, every_kind_of_value_in_both_styles)
{
    written output = {0};
    start(&output, TB_JSON_COMPACT);
    write_sample(output.writer);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==,
                          "{\"name\":\"rock\",\"sizes\":[-1,2,2.5],\"empty\":[],\"none\":{},"
                          "\"flags\":[true,false,null]}");
    start(&output, TB_JSON_INDENTED);
    write_sample(output.writer);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==,
                          "{\n"
                          "  \"name\": \"rock\",\n"
                          "  \"sizes\": [\n"
                          "    -1,\n"
                          "    2,\n"
                          "    2.5\n"
                          "  ],\n"
                          "  \"empty\": [],\n"
                          "  \"none\": {},\n"
                          "  \"flags\": [\n"
                          "    true,\n"
                          "    false,\n"
                          "    null\n"
                          "  ]\n"
                          "}\n");
    /* A document of one scalar. */
    start(&output, TB_JSON_COMPACT);
    (void)tb_json_write_string(output.writer, "", 0);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==, "\"\"");
}

/* Every byte below 0x80 escaped as JSON requires, and read back unchanged. */
TB_TEST(serialize_json_writer, every_ascii_byte_is_escaped_and_reads_back)
{
    char text[128] = {0};
    written output = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    const char *back = NULL;
    size_t length = 0;
    for (size_t index = 0; index < sizeof(text); index++)
    {
        text[index] = (char)index;
    }
    start(&output, TB_JSON_COMPACT);
    TB_TEST_REQUIRE_UINT(tb_json_write_string(output.writer, text, sizeof(text)), ==, TB_JSON_OK);
    finish(&output);
    TB_TEST_EXPECT(tb_str_starts_with(output.text, "\"\\u0000\\u0001"));
    TB_TEST_EXPECT(tb_str_find(output.text, 0, output.length, "\\b\\t\\n\\u000b\\f\\r") !=
                   output.length);
    TB_TEST_EXPECT(tb_str_find(output.text, 0, output.length, " !\\\"#") != output.length);
    TB_TEST_EXPECT(tb_str_find(output.text, 0, output.length, "[\\\\]") != output.length);
    TB_TEST_EXPECT(tb_str_find(output.text, 0, output.length, "/") != output.length);
    TB_TEST_REQUIRE_UINT(tb_json_parse(output.text, output.length, NULL, arena, &root, NULL), ==,
                         TB_JSON_OK);
    back = tb_json_as_text(root, &length);
    TB_TEST_EXPECT_UINT(length, ==, sizeof(text));
    TB_TEST_EXPECT_BYTES(back, text, sizeof(text));
    /* UTF-8 is written as it is. */
    start(&output, TB_JSON_COMPACT);
    TB_TEST_REQUIRE_UINT(tb_json_write_string(output.writer, "\xE6\x97\xA5\xF0\x9F\x98\x80", 7), ==,
                         TB_JSON_OK);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==, "\"\xE6\x97\xA5\xF0\x9F\x98\x80\"");
    tb_arena_destroy(arena);
}

/* Integers at their limits, and random doubles, read back exactly. */
TB_TEST(serialize_json_writer, numbers_read_back_exactly)
{
    written output = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    start(&output, TB_JSON_COMPACT);
    (void)tb_json_write_begin_array(output.writer);
    (void)tb_json_write_int64(output.writer, INT64_MIN);
    (void)tb_json_write_int64(output.writer, INT64_MAX);
    (void)tb_json_write_int64(output.writer, 0);
    (void)tb_json_write_uint64(output.writer, UINT64_MAX);
    (void)tb_json_write_double(output.writer, -0.0);
    (void)tb_json_write_double(output.writer, 5e-324);
    (void)tb_json_write_double(output.writer, 1.7976931348623157e308);
    (void)tb_json_write_end_array(output.writer);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==,
                          "[-9223372036854775808,9223372036854775807,0,18446744073709551615,"
                          "-0.0,5e-324,1.7976931348623157e+308]");
    TB_TEST_REQUIRE_UINT(tb_json_parse(output.text, output.length, NULL, arena, &root, NULL), ==,
                         TB_JSON_OK);
    TB_TEST_EXPECT(tb_json_as_int64(tb_json_at(root, 0), &signed_value) &&
                   signed_value == INT64_MIN);
    TB_TEST_EXPECT(tb_json_as_uint64(tb_json_at(root, 3), &unsigned_value) &&
                   unsigned_value == UINT64_MAX);
    for (size_t trial = 0; trial < 2000; trial++)
    {
        uint64_t bits = 0;
        double value = 0.0;
        double back = 0.0;
        tb_test_random_bytes(&bits, sizeof(bits));
        memcpy(&value, &bits, sizeof(value));
        if (value - value != 0.0)
        {
            continue;
        }
        start(&output, TB_JSON_COMPACT);
        TB_TEST_REQUIRE_UINT(tb_json_write_double(output.writer, value), ==, TB_JSON_OK);
        finish(&output);
        TB_TEST_REQUIRE(tb_json_number_double(output.text, output.length, &back));
        TB_TEST_REQUIRE(memcmp(&back, &value, sizeof(back)) == 0);
    }
    tb_arena_destroy(arena);
}

/* A value JSON cannot hold is refused, and the document goes on. */
TB_TEST(serialize_json_writer, values_json_cannot_hold_are_refused)
{
    written output = {0};
    /* Infinity and a quiet NaN, from their bits. */
    const uint64_t infinity_bits = UINT64_C(0x7FF0000000000000);
    const uint64_t nan_bits = UINT64_C(0x7FF8000000000000);
    double infinity = 0.0;
    double nan = 0.0;
    memcpy(&infinity, &infinity_bits, sizeof(infinity));
    memcpy(&nan, &nan_bits, sizeof(nan));
    start(&output, TB_JSON_COMPACT);
    (void)tb_json_write_begin_array(output.writer);
    TB_TEST_EXPECT_UINT(tb_json_write_double(output.writer, infinity), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_double(output.writer, nan), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_string(output.writer, "\xC3", 1), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_string(output.writer, "\xED\xA0\x80", 3), ==,
                        TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "01", 2), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "1.", 2), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "+1", 2), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "", 0), ==, TB_JSON_INVALID);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "-0.5e+10", 8), ==, TB_JSON_OK);
    TB_TEST_EXPECT_UINT(tb_json_write_number_text(output.writer, "0", 1), ==, TB_JSON_OK);
    (void)tb_json_write_begin_object(output.writer);
    TB_TEST_EXPECT_UINT(tb_json_write_key(output.writer, "\xFF", 1), ==, TB_JSON_INVALID);
    (void)tb_json_write_end_object(output.writer);
    (void)tb_json_write_end_array(output.writer);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==, "[-0.5e+10,0,{}]");
}

static tb_stream_result refusing_write(void *state, const void *bytes, size_t length,
                                       size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)length;
    (void)transferred;
    return TB_STREAM_NO_SPACE;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

TB_TEST(serialize_json_writer, a_sink_that_refuses_is_reported)
{
    static const tb_stream_functions refusing = {.write_bytes = refusing_write,
                                                 .release = nothing_to_release};
    tb_stream sink = {&refusing, NULL};
    tb_json_writer *writer = tb_json_writer_create(&sink, TB_JSON_COMPACT, NULL);
    /* Small: refused at the finish. */
    (void)tb_json_write_null(writer);
    TB_TEST_EXPECT_UINT(tb_json_writer_finish(writer), ==, TB_JSON_OUTPUT_FAILED);
    tb_json_writer_destroy(writer);
    /* Large: refused when the buffer first fills, and every call says so after. */
    writer = tb_json_writer_create(&sink, TB_JSON_COMPACT, NULL);
    (void)tb_json_write_begin_array(writer);
    for (size_t index = 0; index < 2000; index++)
    {
        (void)tb_json_write_string(writer, "some text", 9);
    }
    TB_TEST_EXPECT_UINT(tb_json_write_null(writer), ==, TB_JSON_OUTPUT_FAILED);
    (void)tb_json_write_end_array(writer);
    TB_TEST_EXPECT_UINT(tb_json_writer_finish(writer), ==, TB_JSON_OUTPUT_FAILED);
    tb_json_writer_destroy(writer);
    tb_json_writer_destroy(NULL);
}

/* A document as deep as the reader allows writes without recursion. */
TB_TEST(serialize_json_writer, a_deep_document_writes_back_the_same)
{
    enum
    {
        DEPTH = 256
    };
    char text[2 * DEPTH + 8] = {0};
    written output = {0};
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    for (size_t index = 0; index < DEPTH; index++)
    {
        text[index] = '[';
        text[2 * DEPTH - 1 - index] = ']';
    }
    TB_TEST_REQUIRE_UINT(tb_json_parse(text, 2 * DEPTH, NULL, arena, &root, NULL), ==, TB_JSON_OK);
    start(&output, TB_JSON_COMPACT);
    TB_TEST_REQUIRE_UINT(tb_json_write_value(output.writer, root), ==, TB_JSON_OK);
    finish(&output);
    TB_TEST_EXPECT_STRING(output.text, ==, text);
    tb_arena_destroy(arena);
}

static void writer_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    /* Everything comes from a fixed arena, so a trap leaves nothing on the heap. */
    static unsigned char memory[32768];
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_stream sink = {0};
    tb_json_writer *writer = NULL;
    tb_stream_open_buffer(&allocator, &sink);
    writer = tb_json_writer_create(&sink, TB_JSON_COMPACT, &allocator);
    switch (choice)
    {
    case 0:
        (void)tb_json_write_key(writer, "a", 1);
        break;
    case 1:
        (void)tb_json_write_begin_object(writer);
        (void)tb_json_write_null(writer);
        break;
    case 2:
        (void)tb_json_write_begin_array(writer);
        (void)tb_json_write_end_object(writer);
        break;
    case 3:
        (void)tb_json_write_null(writer);
        (void)tb_json_write_null(writer);
        break;
    case 4:
        (void)tb_json_write_begin_array(writer);
        (void)tb_json_writer_finish(writer);
        break;
    case 5:
        (void)tb_json_write_begin_object(writer);
        (void)tb_json_write_key(writer, "a", 1);
        (void)tb_json_write_key(writer, "b", 1);
        break;
    case 6:
        (void)tb_json_write_begin_object(writer);
        (void)tb_json_write_key(writer, "a", 1);
        (void)tb_json_write_end_object(writer);
        break;
    default:
        (void)tb_json_write_null(writer);
        (void)tb_json_writer_finish(writer);
        (void)tb_json_write_null(writer);
        break;
    }
}

TB_TEST(serialize_json_writer, structure_misuse_is_a_contract_violation)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 8; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(writer_contract, &choice, "assertion failed");
    }
}
