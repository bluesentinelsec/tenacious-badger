/* JSON reading against JSONTestSuite's parsing corpus, an independent set of
 * texts that RFC 8259 requires a parser to accept (y_), to reject (n_), or
 * leaves to the parser (i_). The reader and the document must agree on every
 * text, the accepted ones must survive a trip through the writer, and the
 * choices made for the i_ texts are pinned here, so a change to one is
 * deliberate. */
#include "badger/arena.h"
#include "badger/json.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "tb_test.h"
#include "test_data.h"

#include <string.h>

extern const tb_test_file tb_json_suite_files[];
extern const size_t tb_json_suite_files_count;

/* Every relaxation at once, to show that none of them breaks on any text. */
static const tb_json_options relaxed = {
    .flags = TB_JSON_ALLOW_COMMENTS | TB_JSON_ALLOW_TRAILING_COMMAS | TB_JSON_ALLOW_DUPLICATE_KEYS,
};

static bool has_prefix(const char *name, const char *prefix)
{
    const size_t length = tb_str_length(prefix);
    return tb_str_length(name) >= length && memcmp(name, prefix, length) == 0;
}

/* Read a text to its end with the reader; return the result. */
static tb_json_result read_events(const tb_test_file *file, const tb_json_options *options)
{
    tb_json_reader *const reader =
        tb_json_reader_create((const char *)file->bytes, file->length, options, NULL);
    tb_json_result result = TB_JSON_OK;
    tb_json_error error = {0};
    for (;;)
    {
        tb_json_token token = {0};
        result = tb_json_reader_next(reader, &token);
        if (result != TB_JSON_OK || token.event == TB_JSON_EVENT_END)
        {
            break;
        }
    }
    tb_json_reader_error(reader, &error);
    TB_TEST_EXPECT_UINT(error.result, ==, result);
    if (result != TB_JSON_OK)
    {
        TB_TEST_EXPECT(error.reason[0] != '\0');
        TB_TEST_EXPECT(error.line >= 1);
        TB_TEST_EXPECT(error.column >= 1);
        TB_TEST_EXPECT(error.offset <= file->length);
    }
    tb_json_reader_destroy(reader);
    return result;
}

/* Read a text both ways, require them to agree, and return the document's
 * result. The reader keeps no keys, so only the document refuses a duplicate. */
static tb_json_result read_both_ways(const tb_test_file *file, const tb_json_options *options)
{
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    const tb_json_result events = read_events(file, options);
    const tb_json_result document =
        tb_json_parse((const char *)file->bytes, file->length, options, arena, &root, NULL);
    if (document == TB_JSON_DUPLICATE_KEY)
    {
        TB_TEST_EXPECT_UINT(events, ==, TB_JSON_OK);
    }
    else
    {
        TB_TEST_EXPECT_UINT(events, ==, document);
    }
    TB_TEST_EXPECT((document == TB_JSON_OK) == (root != NULL));
    tb_arena_destroy(arena);
    return document;
}

/* Write a document compactly into `sink`, which the caller closes. */
static const char *write_compact(const tb_json_value *root, tb_stream *sink, size_t *length)
{
    tb_json_writer *writer = NULL;
    tb_stream_open_buffer(NULL, sink);
    writer = tb_json_writer_create(sink, TB_JSON_COMPACT, NULL);
    TB_TEST_REQUIRE_UINT(tb_json_write_value(writer, root), ==, TB_JSON_OK);
    TB_TEST_REQUIRE_UINT(tb_json_writer_finish(writer), ==, TB_JSON_OK);
    tb_json_writer_destroy(writer);
    return (const char *)tb_stream_buffer_bytes(sink, length);
}

/* Parse, write, parse what was written, and write again: the two writings
 * are equal byte for byte. */
static void expect_round_trip(const tb_test_file *file)
{
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    const tb_json_value *again = NULL;
    tb_stream first = {0};
    tb_stream second = {0};
    size_t first_length = 0;
    size_t second_length = 0;
    const char *first_text = NULL;
    const char *second_text = NULL;
    TB_TEST_REQUIRE_UINT(
        tb_json_parse((const char *)file->bytes, file->length, &relaxed, arena, &root, NULL), ==,
        TB_JSON_OK);
    first_text = write_compact(root, &first, &first_length);
    TB_TEST_REQUIRE_UINT(tb_json_parse(first_text, first_length, &relaxed, arena, &again, NULL), ==,
                         TB_JSON_OK);
    second_text = write_compact(again, &second, &second_length);
    TB_TEST_EXPECT_UINT(second_length, ==, first_length);
    if (second_length == first_length)
    {
        TB_TEST_EXPECT_BYTES(second_text, first_text, first_length);
    }
    (void)tb_stream_close(&second);
    (void)tb_stream_close(&first);
    tb_arena_destroy(arena);
}

TB_TEST(serialize_json_suite, the_corpus_is_all_there)
{
    size_t accepted = 0;
    size_t rejected = 0;
    size_t either = 0;
    for (size_t index = 0; index < tb_json_suite_files_count; index++)
    {
        const char *const name = tb_json_suite_files[index].name;
        if (has_prefix(name, "y_"))
        {
            accepted++;
        }
        else if (has_prefix(name, "n_"))
        {
            rejected++;
        }
        else
        {
            TB_TEST_EXPECT(has_prefix(name, "i_"));
            either++;
        }
    }
    TB_TEST_EXPECT_UINT(accepted, ==, 95);
    TB_TEST_EXPECT_UINT(rejected, ==, 188);
    TB_TEST_EXPECT_UINT(either, ==, 35);
}

/* Every text that must be accepted is, both ways, and writes back exactly.
 * Duplicate keys are valid JSON but refused by default, so the two texts
 * that have them are accepted only when duplicates are allowed. */
TB_TEST(serialize_json_suite, every_text_that_must_be_accepted_is)
{
    for (size_t index = 0; index < tb_json_suite_files_count; index++)
    {
        const tb_test_file *const file = &tb_json_suite_files[index];
        if (!has_prefix(file->name, "y_"))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        if (has_prefix(file->name, "y_object_duplicated_key"))
        {
            TB_TEST_EXPECT_UINT(read_both_ways(file, NULL), ==, TB_JSON_DUPLICATE_KEY);
        }
        else
        {
            TB_TEST_EXPECT_UINT(read_both_ways(file, NULL), ==, TB_JSON_OK);
        }
        TB_TEST_EXPECT_UINT(read_both_ways(file, &relaxed), ==, TB_JSON_OK);
        expect_round_trip(file);
    }
}

/* Every text that must be rejected is, both ways, with where and why. With
 * the relaxations some become valid, such as a trailing comma; none breaks. */
TB_TEST(serialize_json_suite, every_text_that_must_be_rejected_is)
{
    for (size_t index = 0; index < tb_json_suite_files_count; index++)
    {
        const tb_test_file *const file = &tb_json_suite_files[index];
        if (!has_prefix(file->name, "n_"))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        TB_TEST_EXPECT_UINT(read_both_ways(file, NULL), !=, TB_JSON_OK);
        (void)read_both_ways(file, &relaxed);
    }
}

/* What RFC 8259 leaves to the parser, and what this one does. */
typedef struct choice
{
    const char *name;
    tb_json_result result;
} choice;

static const choice choices[] = {
    /* Numbers beyond a double's range are valid JSON; their text is kept, and
     * converting them is the caller's question. */
    {"i_number_double_huge_neg_exp.json", TB_JSON_OK},
    {"i_number_huge_exp.json", TB_JSON_OK},
    {"i_number_neg_int_huge_exp.json", TB_JSON_OK},
    {"i_number_pos_double_huge_exp.json", TB_JSON_OK},
    {"i_number_real_neg_overflow.json", TB_JSON_OK},
    {"i_number_real_pos_overflow.json", TB_JSON_OK},
    {"i_number_real_underflow.json", TB_JSON_OK},
    {"i_number_too_big_neg_int.json", TB_JSON_OK},
    {"i_number_too_big_pos_int.json", TB_JSON_OK},
    {"i_number_very_big_negative_int.json", TB_JSON_OK},
    /* Strings must decode to UTF-8, so an escaped surrogate without its
     * partner, bytes that are not UTF-8, and text in other encodings are
     * refused. */
    {"i_object_key_lone_2nd_surrogate.json", TB_JSON_MALFORMED},
    {"i_string_1st_surrogate_but_2nd_missing.json", TB_JSON_MALFORMED},
    {"i_string_1st_valid_surrogate_2nd_invalid.json", TB_JSON_MALFORMED},
    {"i_string_UTF-16LE_with_BOM.json", TB_JSON_MALFORMED},
    {"i_string_UTF-8_invalid_sequence.json", TB_JSON_MALFORMED},
    {"i_string_UTF8_surrogate_U+D800.json", TB_JSON_MALFORMED},
    {"i_string_incomplete_surrogate_and_escape_valid.json", TB_JSON_MALFORMED},
    {"i_string_incomplete_surrogate_pair.json", TB_JSON_MALFORMED},
    {"i_string_incomplete_surrogates_escape_valid.json", TB_JSON_MALFORMED},
    {"i_string_invalid_lonely_surrogate.json", TB_JSON_MALFORMED},
    {"i_string_invalid_surrogate.json", TB_JSON_MALFORMED},
    {"i_string_invalid_utf-8.json", TB_JSON_MALFORMED},
    {"i_string_inverted_surrogates_U+1D11E.json", TB_JSON_MALFORMED},
    {"i_string_iso_latin_1.json", TB_JSON_MALFORMED},
    {"i_string_lone_second_surrogate.json", TB_JSON_MALFORMED},
    {"i_string_lone_utf8_continuation_byte.json", TB_JSON_MALFORMED},
    {"i_string_not_in_unicode_range.json", TB_JSON_MALFORMED},
    {"i_string_overlong_sequence_2_bytes.json", TB_JSON_MALFORMED},
    {"i_string_overlong_sequence_6_bytes.json", TB_JSON_MALFORMED},
    {"i_string_overlong_sequence_6_bytes_null.json", TB_JSON_MALFORMED},
    {"i_string_truncated-utf-8.json", TB_JSON_MALFORMED},
    {"i_string_utf16BE_no_BOM.json", TB_JSON_MALFORMED},
    {"i_string_utf16LE_no_BOM.json", TB_JSON_MALFORMED},
    /* Deeper than the default limit of 256. */
    {"i_structure_500_nested_arrays.json", TB_JSON_TOO_DEEP},
    /* RFC 8259 forbids writing a byte order mark; this parser does not skip one. */
    {"i_structure_UTF-8_BOM_empty_object.json", TB_JSON_MALFORMED},
};

TB_TEST(serialize_json_suite, every_text_left_to_the_parser_has_a_pinned_outcome)
{
    size_t seen = 0;
    for (size_t index = 0; index < tb_json_suite_files_count; index++)
    {
        const tb_test_file *const file = &tb_json_suite_files[index];
        const choice *found = NULL;
        if (!has_prefix(file->name, "i_"))
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", file->name);
        for (size_t entry = 0; entry < sizeof(choices) / sizeof(choices[0]); entry++)
        {
            if (tb_str_equal(choices[entry].name, file->name))
            {
                found = &choices[entry];
            }
        }
        TB_TEST_REQUIRE(found != NULL);
        TB_TEST_EXPECT_STRING(tb_json_result_name(read_both_ways(file, NULL)), ==,
                              tb_json_result_name(found->result));
        if (found->result == TB_JSON_OK)
        {
            expect_round_trip(file);
        }
        seen++;
    }
    TB_TEST_EXPECT_UINT(seen, ==, sizeof(choices) / sizeof(choices[0]));
}
