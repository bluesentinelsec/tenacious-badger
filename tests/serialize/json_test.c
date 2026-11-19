/* JSON reading: every kind of value, every escape and number form RFC 8259
 * allows and many it does not, the limits on nesting and length, the relaxed
 * forms, documents with their lookups, and hostile text: every truncation,
 * random mutation, and errors reported where they are. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "tb_test.h"

#include <string.h>

static tb_json_result read_all(const char *text, const tb_json_options *options, size_t *events,
                               tb_json_error *error)
{
    tb_json_reader *const reader = tb_json_reader_create(text, tb_str_length(text), options, NULL);
    tb_json_result result = TB_JSON_OK;
    *events = 0;
    for (;;)
    {
        tb_json_token token = {0};
        result = tb_json_reader_next(reader, &token);
        if (result != TB_JSON_OK || token.event == TB_JSON_EVENT_END)
        {
            break;
        }
        (*events)++;
    }
    tb_json_reader_error(reader, error);
    tb_json_reader_destroy(reader);
    return result;
}

static void expect_valid(const char *text, size_t events)
{
    size_t counted = 0;
    tb_json_error error = {0};
    TB_TEST_CONTEXT("%s", text);
    TB_TEST_EXPECT_UINT(read_all(text, NULL, &counted, &error), ==, TB_JSON_OK);
    TB_TEST_EXPECT_UINT(counted, ==, events);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
}

static void expect_error(const char *text, const tb_json_options *options, tb_json_result result,
                         size_t line, size_t column)
{
    size_t counted = 0;
    tb_json_error error = {0};
    TB_TEST_CONTEXT("%s", text);
    TB_TEST_EXPECT_UINT(read_all(text, options, &counted, &error), ==, result);
    TB_TEST_EXPECT_UINT(error.result, ==, result);
    TB_TEST_EXPECT_UINT(error.line, ==, line);
    TB_TEST_EXPECT_UINT(error.column, ==, column);
    TB_TEST_EXPECT(error.reason[0] != '\0');
}

/* -------------------------------------------------------------------------
 * The reader.
 * ------------------------------------------------------------------------- */

TB_TEST(serialize_json, every_kind_of_value_alone_and_nested)
{
    expect_valid("null", 1);
    expect_valid("true", 1);
    expect_valid("false", 1);
    expect_valid("0", 1);
    expect_valid("\"\"", 1);
    expect_valid("[]", 2);
    expect_valid("{}", 2);
    expect_valid("[[]]", 4);
    expect_valid("{\"\":{}}", 5);
    expect_valid("[null,true,false,-1.5e3,\"s\",[],{}]", 11);
    expect_valid("{\"a\":[1,{\"b\":null}],\"c\":\"d\"}", 12);
    /* Space wherever RFC 8259 allows it: around every token. */
    expect_valid(
        " \t\r\n{ \t\r\n\"a\" \t\r\n: \t\r\n[ \t\r\n1 \t\r\n, \t\r\n2 \t\r\n] \t\r\n} \t\r\n", 7);
}

/* Read one string and decode it. */
static void expect_string(const char *text, const char *decoded, size_t decoded_length)
{
    tb_json_reader *const reader = tb_json_reader_create(text, tb_str_length(text), NULL, NULL);
    tb_json_token token = {0};
    char output[64] = {0};
    size_t length = 0;
    TB_TEST_CONTEXT("%s", text);
    TB_TEST_REQUIRE_UINT(tb_json_reader_next(reader, &token), ==, TB_JSON_OK);
    TB_TEST_REQUIRE_UINT(token.event, ==, TB_JSON_EVENT_STRING);
    TB_TEST_REQUIRE_UINT(token.length, <=, sizeof(output));
    length = tb_json_unescape(token.text, token.length, output);
    TB_TEST_EXPECT_UINT(length, ==, decoded_length);
    TB_TEST_EXPECT_BYTES(output, decoded, decoded_length);
    TB_TEST_EXPECT_UINT(tb_json_reader_next(reader, &token), ==, TB_JSON_OK);
    TB_TEST_EXPECT_UINT(token.event, ==, TB_JSON_EVENT_END);
    tb_json_reader_destroy(reader);
}

TB_TEST(serialize_json, strings_decode_every_escape_and_pair)
{
    expect_string("\"plain\"", "plain", 5);
    expect_string("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "\"\\/\b\f\n\r\t", 8);
    expect_string("\"\\u0041\\u00e9\\u20AC\"", "A\xC3\xA9\xE2\x82\xAC", 6);
    expect_string("\"\\u0000\"", "\0", 1);
    /* A surrogate pair is one scalar beyond the basic plane. */
    expect_string("\"\\uD83D\\uDE00\"", "\xF0\x9F\x98\x80", 4);
    /* UTF-8 passes through unchanged. */
    expect_string("\"\xE6\x97\xA5\xF0\x9F\x98\x80\"", "\xE6\x97\xA5\xF0\x9F\x98\x80", 7);
}

TB_TEST(serialize_json, malformed_strings_are_rejected_where_they_go_wrong)
{
    expect_error("\"abc", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("\"a\\x\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\\u12\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\\uDE00\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\\uD83D\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\\uD83D\\u0041\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\tb\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"a\nb\"", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("\"\xC3\"", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("\"\xED\xA0\x80\"", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("\"\xFF\"", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("'single'", NULL, TB_JSON_MALFORMED, 1, 1);
}

TB_TEST(serialize_json, numbers_read_exactly_at_every_limit)
{
    static const struct
    {
        const char *text;
        bool is_int64;
        int64_t int64;
        bool is_uint64;
        uint64_t uint64;
        double value;
    } rows[] = {
        {"0", true, 0, true, 0, 0.0},
        {"-0", true, 0, true, 0, -0.0},
        {"9223372036854775807", true, INT64_MAX, true, (uint64_t)INT64_MAX, 9223372036854775807.0},
        {"9223372036854775808", false, 0, true, (uint64_t)INT64_MAX + 1, 9223372036854775808.0},
        {"-9223372036854775808", true, INT64_MIN, false, 0, -9223372036854775808.0},
        {"-9223372036854775809", false, 0, false, 0, -9223372036854775809.0},
        {"18446744073709551615", false, 0, true, UINT64_MAX, 18446744073709551615.0},
        {"18446744073709551616", false, 0, false, 0, 18446744073709551616.0},
        {"9007199254740992", true, 9007199254740992, true, 9007199254740992u, 9007199254740992.0},
        {"9007199254740993", true, 9007199254740993, true, 9007199254740993u, 9007199254740992.0},
        {"1.5", false, 0, false, 0, 1.5},
        {"1e2", false, 0, false, 0, 100.0},
        {"-2.5E-3", false, 0, false, 0, -0.0025},
        {"1.7976931348623157e308", false, 0, false, 0, 1.7976931348623157e308},
        {"4.9406564584124654e-324", false, 0, false, 0, 4.9406564584124654e-324},
        {"2.2250738585072014e-308", false, 0, false, 0, 2.2250738585072014e-308},
    };
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        const char *const text = rows[row].text;
        const size_t length = tb_str_length(text);
        int64_t signed_value = 0;
        uint64_t unsigned_value = 0;
        double value = 0.0;
        TB_TEST_CONTEXT("%s", text);
        expect_valid(text, 1);
        TB_TEST_EXPECT(tb_json_number_int64(text, length, &signed_value) == rows[row].is_int64);
        if (rows[row].is_int64)
        {
            TB_TEST_EXPECT_INT(signed_value, ==, rows[row].int64);
        }
        TB_TEST_EXPECT(tb_json_number_uint64(text, length, &unsigned_value) == rows[row].is_uint64);
        if (rows[row].is_uint64)
        {
            TB_TEST_EXPECT_UINT(unsigned_value, ==, rows[row].uint64);
        }
        TB_TEST_REQUIRE(tb_json_number_double(text, length, &value));
        TB_TEST_EXPECT_FLOAT(value, ==, rows[row].value);
    }
    /* Too large for a double is not a double. */
    {
        double value = 0.0;
        TB_TEST_EXPECT(!tb_json_number_double("1e400", 5, &value));
        TB_TEST_EXPECT(tb_json_number_double("1e-400", 6, &value));
        TB_TEST_EXPECT_FLOAT(value, ==, 0.0);
    }
}

TB_TEST(serialize_json, numbers_of_forms_rfc_8259_refuses)
{
    expect_error("01", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("-01", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("1.", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error(".1", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("+1", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("1e", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("1e+", NULL, TB_JSON_MALFORMED, 1, 4);
    expect_error("-", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("0x10", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("Infinity", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("NaN", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("1 2", NULL, TB_JSON_MALFORMED, 1, 3);
}

TB_TEST(serialize_json, structure_errors_say_where)
{
    expect_error("", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("   ", NULL, TB_JSON_MALFORMED, 1, 4);
    expect_error("[1,2", NULL, TB_JSON_MALFORMED, 1, 5);
    expect_error("[1,]", NULL, TB_JSON_MALFORMED, 1, 4);
    expect_error("{\"a\":1,}", NULL, TB_JSON_MALFORMED, 1, 8);
    expect_error("{\"a\":}", NULL, TB_JSON_MALFORMED, 1, 6);
    expect_error("{\"a\" 1}", NULL, TB_JSON_MALFORMED, 1, 6);
    expect_error("{1:2}", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("[1}", NULL, TB_JSON_MALFORMED, 1, 3);
    expect_error("{\"a\":1]", NULL, TB_JSON_MALFORMED, 1, 7);
    expect_error("[,1]", NULL, TB_JSON_MALFORMED, 1, 2);
    expect_error("[1 2]", NULL, TB_JSON_MALFORMED, 1, 4);
    expect_error("nul", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("truth", NULL, TB_JSON_MALFORMED, 1, 1);
    expect_error("trueish", NULL, TB_JSON_MALFORMED, 1, 5);
    expect_error("{}\n\n  x", NULL, TB_JSON_MALFORMED, 3, 3);
    expect_error("[\n1,\n2,\n]", NULL, TB_JSON_MALFORMED, 4, 1);
    expect_error("// note\n1", NULL, TB_JSON_MALFORMED, 1, 1);
}

TB_TEST(serialize_json, relaxed_forms_are_allowed_only_when_asked)
{
    tb_json_options relaxed = {0};
    size_t events = 0;
    tb_json_error error = {0};
    relaxed.flags = TB_JSON_ALLOW_COMMENTS | TB_JSON_ALLOW_TRAILING_COMMAS;
    TB_TEST_EXPECT_UINT(read_all("// heading\n{ /* a */ \"a\": [1, 2,], // end\n\"b\": 3, }\n",
                                 &relaxed, &events, &error),
                        ==, TB_JSON_OK);
    TB_TEST_EXPECT_UINT(events, ==, 9);
    expect_error("[1 /* open", &relaxed, TB_JSON_MALFORMED, 1, 4);
    /* A trailing comma still needs something before it. */
    expect_error("[,]", &relaxed, TB_JSON_MALFORMED, 1, 2);
    expect_error("{\"a\":1,,}", &relaxed, TB_JSON_MALFORMED, 1, 8);
    relaxed.flags = 64;
    expect_error("1", &relaxed, TB_JSON_INVALID, 1, 1);
}

static void nest(char *text, size_t depth)
{
    for (size_t index = 0; index < depth; index++)
    {
        text[index] = '[';
        text[2 * depth - 1 - index] = ']';
    }
    text[2 * depth] = '\0';
}

TB_TEST(serialize_json, nesting_and_length_stop_at_their_limits)
{
    enum
    {
        DEEP = 300
    };
    char text[2 * (DEEP + 1) + 1] = {0};
    tb_json_options options = {0};
    size_t events = 0;
    tb_json_error error = {0};
    /* The default limit, exactly, and one past it. */
    nest(text, TB_JSON_DEFAULT_MAX_DEPTH);
    TB_TEST_EXPECT_UINT(read_all(text, NULL, &events, &error), ==, TB_JSON_OK);
    nest(text, TB_JSON_DEFAULT_MAX_DEPTH + 1);
    TB_TEST_EXPECT_UINT(read_all(text, NULL, &events, &error), ==, TB_JSON_TOO_DEEP);
    TB_TEST_EXPECT_UINT(error.offset, ==, TB_JSON_DEFAULT_MAX_DEPTH);
    options.max_depth = 3;
    nest(text, 3);
    TB_TEST_EXPECT_UINT(read_all(text, &options, &events, &error), ==, TB_JSON_OK);
    nest(text, 4);
    TB_TEST_EXPECT_UINT(read_all(text, &options, &events, &error), ==, TB_JSON_TOO_DEEP);
    /* A limit of a deep document's whole length, and one byte less. */
    options.max_depth = 0;
    options.max_length = 8;
    TB_TEST_EXPECT_UINT(read_all("[[[[]]]]", &options, &events, &error), ==, TB_JSON_OK);
    options.max_length = 7;
    TB_TEST_EXPECT_UINT(read_all("[[[[]]]]", &options, &events, &error), ==, TB_JSON_TOO_LARGE);
}

/* -------------------------------------------------------------------------
 * Documents.
 * ------------------------------------------------------------------------- */

TB_TEST(serialize_json, a_document_holds_every_value_after_the_text_is_gone)
{
    static const char source[] = "{\"name\": \"rock \\u00e9\", \"size\": [1, 2.5, -3],"
                                 " \"solid\": true, \"note\": null, \"empty\": {},"
                                 " \"big\": 18446744073709551615}";
    tb_arena *const arena = tb_arena_create(0);
    char *const text = tb_malloc(sizeof(source));
    const tb_json_value *root = NULL;
    const tb_json_value *found = NULL;
    tb_json_error error = {0};
    const char *string = NULL;
    const char *key = NULL;
    size_t length = 0;
    int64_t integer = 0;
    uint64_t unsigned_integer = 0;
    double number = 0.0;
    bool flag = false;
    memcpy(text, source, sizeof(source));
    TB_TEST_REQUIRE_UINT(tb_json_parse(text, sizeof(source) - 1, NULL, arena, &root, &error), ==,
                         TB_JSON_OK);
    /* The document does not borrow the text. */
    memset(text, 'x', sizeof(source) - 1);
    tb_free(text);
    TB_TEST_REQUIRE_UINT(tb_json_type_of(root), ==, TB_JSON_OBJECT);
    TB_TEST_EXPECT_UINT(tb_json_count(root), ==, 6);
    found = tb_json_member_at(root, 0, &key, &length);
    TB_TEST_EXPECT_STRING(key, ==, "name");
    string = tb_json_as_text(found, &length);
    TB_TEST_EXPECT_UINT(length, ==, 7);
    TB_TEST_EXPECT_STRING(string, ==, "rock \xC3\xA9");
    found = tb_json_find(root, "size", 4);
    TB_TEST_REQUIRE(found != NULL);
    TB_TEST_EXPECT_UINT(tb_json_count(found), ==, 3);
    TB_TEST_EXPECT(tb_json_as_int64(tb_json_at(found, 0), &integer) && integer == 1);
    TB_TEST_EXPECT(!tb_json_as_int64(tb_json_at(found, 1), &integer));
    TB_TEST_EXPECT(tb_json_as_double(tb_json_at(found, 1), &number));
    TB_TEST_EXPECT_FLOAT(number, ==, 2.5);
    TB_TEST_EXPECT(tb_json_as_int64(tb_json_at(found, 2), &integer) && integer == -3);
    string = tb_json_as_text(tb_json_at(found, 1), &length);
    TB_TEST_EXPECT_STRING(string, ==, "2.5");
    TB_TEST_EXPECT(tb_json_as_bool(tb_json_find(root, "solid", 5), &flag) && flag);
    TB_TEST_EXPECT_UINT(tb_json_type_of(tb_json_find(root, "note", 4)), ==, TB_JSON_NULL);
    TB_TEST_EXPECT_UINT(tb_json_count(tb_json_find(root, "empty", 5)), ==, 0);
    TB_TEST_EXPECT(tb_json_as_uint64(tb_json_find(root, "big", 3), &unsigned_integer) &&
                   unsigned_integer == UINT64_MAX);
    TB_TEST_EXPECT_POINTER(tb_json_find(root, "absent", 6), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_json_find(root, "siz", 3), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_json_find(found, "a", 1), ==, NULL);
    /* Asking a value for another type's contents is an answer, not a fault. */
    TB_TEST_EXPECT(!tb_json_as_bool(found, &flag));
    TB_TEST_EXPECT_POINTER(tb_json_as_text(found, &length), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_json_count(tb_json_find(root, "note", 4)), ==, 0);
    tb_arena_destroy(arena);
}

/* An object of many keys finds each of them, and each absent neighbour not. */
TB_TEST(serialize_json, lookups_in_a_large_object_find_every_key)
{
    enum
    {
        KEYS = 2000
    };
    tb_arena *const arena = tb_arena_create(0);
    char *const text = tb_malloc(KEYS * 24 + 2);
    size_t length = 0;
    const tb_json_value *root = NULL;
    text[length++] = '{';
    for (size_t index = 0; index < KEYS; index++)
    {
        /* Keys in an order that is not sorted. */
        const size_t key = (index * 7919) % KEYS;
        length +=
            tb_fmt_buffer(text + length, 24, "%s\"k%zu\":%zu", index == 0 ? "" : ",", key, key);
    }
    text[length++] = '}';
    TB_TEST_REQUIRE_UINT(tb_json_parse(text, length, NULL, arena, &root, NULL), ==, TB_JSON_OK);
    TB_TEST_REQUIRE_UINT(tb_json_count(root), ==, KEYS);
    for (size_t key = 0; key < KEYS; key++)
    {
        char name[24] = {0};
        const size_t name_length = tb_fmt_buffer(name, sizeof(name), "k%zu", key);
        const tb_json_value *const found = tb_json_find(root, name, name_length);
        int64_t value = 0;
        TB_TEST_REQUIRE(found != NULL);
        TB_TEST_REQUIRE(tb_json_as_int64(found, &value) && (size_t)value == key);
        name[name_length] = 'x';
        TB_TEST_REQUIRE_POINTER(tb_json_find(root, name, name_length + 1), ==, NULL);
    }
    tb_free(text);
    tb_arena_destroy(arena);
}

TB_TEST(serialize_json, duplicate_keys_are_refused_unless_allowed)
{
    static const char text[] = "{\"a\": 1, \"b\": {\"x\": 1}, \"a\": 2}";
    tb_arena *const arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    tb_json_options options = {0};
    tb_json_error error = {0};
    int64_t value = 0;
    TB_TEST_EXPECT_UINT(tb_json_parse(text, sizeof(text) - 1, NULL, arena, &root, &error), ==,
                        TB_JSON_DUPLICATE_KEY);
    TB_TEST_EXPECT_POINTER(root, ==, NULL);
    TB_TEST_EXPECT_UINT(error.offset, ==, 24);
    TB_TEST_EXPECT_UINT(error.column, ==, 25);
    options.flags = TB_JSON_ALLOW_DUPLICATE_KEYS;
    TB_TEST_REQUIRE_UINT(tb_json_parse(text, sizeof(text) - 1, &options, arena, &root, &error), ==,
                         TB_JSON_OK);
    TB_TEST_EXPECT_UINT(tb_json_count(root), ==, 3);
    TB_TEST_EXPECT(tb_json_as_int64(tb_json_find(root, "a", 1), &value) && value == 2);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    /* Keys differing only in an escaped form are the same key. */
    TB_TEST_EXPECT_UINT(tb_json_parse("{\"\\u0061\":1,\"a\":2}", 19, NULL, arena, &root, &error),
                        ==, TB_JSON_DUPLICATE_KEY);
    tb_arena_destroy(arena);
}

TB_TEST(serialize_json, a_full_arena_is_reported)
{
    unsigned char memory[512] = {0};
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_json_value *root = NULL;
    tb_json_error error = {0};
    char text[4096] = {0};
    size_t length = 0;
    text[length++] = '[';
    for (size_t index = 0; index < 400; index++)
    {
        length += tb_fmt_buffer(text + length, sizeof(text) - length, "%s%zu",
                                index == 0 ? "" : ",", index);
    }
    text[length++] = ']';
    TB_TEST_EXPECT_UINT(tb_json_parse(text, length, NULL, arena, &root, &error), ==,
                        TB_JSON_NO_MEMORY);
    TB_TEST_EXPECT_POINTER(root, ==, NULL);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the arena is full");
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * Random documents and hostile text.
 * ------------------------------------------------------------------------- */

typedef struct generated
{
    char text[1 << 16];
    size_t length;
    size_t events;
} generated;

static void put(generated *output, const char *text)
{
    const size_t length = tb_str_length(text);
    TB_TEST_REQUIRE_UINT(output->length + length, <, sizeof(output->text));
    memcpy(output->text + output->length, text, length);
    output->length += length;
}

/* A random document of bounded size, with random space, written without
 * recursion: a stack of open containers and how many values each has left. */
static void generate(generated *output)
{
    static const char *const scalars[] = {"null",
                                          "true",
                                          "false",
                                          "0",
                                          "-12.5e-3",
                                          "18446744073709551615",
                                          "\"\"",
                                          "\"a\\n\\u00e9\"",
                                          "\"\xF0\x9F\x98\x80\"",
                                          "-0"};
    static const char *const spaces[] = {"", " ", "\n", "\t ", "\r\n"};
    char kinds[32] = {0};
    size_t left[32] = {0};
    bool first[32] = {false};
    size_t depth = 0;
    output->length = 0;
    output->events = 0;
    for (;;)
    {
        if (depth > 0 && left[depth - 1] == 0)
        {
            put(output, spaces[tb_test_random_below(5)]);
            put(output, kinds[depth - 1] == '{' ? "}" : "]");
            output->events++;
            depth--;
            if (depth == 0)
            {
                break;
            }
            continue;
        }
        if (depth > 0)
        {
            left[depth - 1]--;
            if (!first[depth - 1])
            {
                put(output, ",");
            }
            first[depth - 1] = false;
            if (kinds[depth - 1] == '{')
            {
                /* Keys count down, so they never repeat within an object. */
                char key[24] = {0};
                (void)tb_fmt_buffer(key, sizeof(key), "\"k%zu\":", left[depth - 1]);
                put(output, spaces[tb_test_random_below(5)]);
                put(output, key);
                output->events++;
            }
        }
        put(output, spaces[tb_test_random_below(5)]);
        if (depth < TB_TEST_COUNT_OF(kinds) - 2 && tb_test_random_below(3) == 0)
        {
            const bool object = tb_test_random_below(2) == 0;
            kinds[depth] = object ? '{' : '[';
            left[depth] = (size_t)tb_test_random_below(6);
            first[depth] = true;
            depth++;
            put(output, object ? "{" : "[");
            output->events++;
            continue;
        }
        put(output, scalars[tb_test_random_below(TB_TEST_COUNT_OF(scalars))]);
        output->events++;
        if (depth == 0)
        {
            break;
        }
    }
    put(output, spaces[tb_test_random_below(5)]);
}

/* Count a document's events by walking it, without recursion. */
static size_t count_tree_events(const tb_json_value *root)
{
    const tb_json_value *stack[64] = {NULL};
    size_t positions[64] = {0};
    size_t depth = 0;
    size_t events = 1;
    if (tb_json_type_of(root) != TB_JSON_ARRAY && tb_json_type_of(root) != TB_JSON_OBJECT)
    {
        return 1;
    }
    stack[0] = root;
    depth = 1;
    while (depth > 0)
    {
        const tb_json_value *const top = stack[depth - 1];
        const size_t at = positions[depth - 1];
        const tb_json_value *child = NULL;
        if (at == tb_json_count(top))
        {
            events++;
            depth--;
            continue;
        }
        positions[depth - 1]++;
        if (tb_json_type_of(top) == TB_JSON_OBJECT)
        {
            const char *key = NULL;
            size_t key_length = 0;
            child = tb_json_member_at(top, at, &key, &key_length);
            events++;
        }
        else
        {
            child = tb_json_at(top, at);
        }
        events++;
        if (tb_json_type_of(child) == TB_JSON_ARRAY || tb_json_type_of(child) == TB_JSON_OBJECT)
        {
            stack[depth] = child;
            positions[depth] = 0;
            depth++;
        }
    }
    return events;
}

TB_TEST(serialize_json, random_documents_read_the_same_both_ways)
{
    static generated text;
    for (size_t trial = 0; trial < 300; trial++)
    {
        tb_arena *const arena = tb_arena_create(0);
        const tb_json_value *root = NULL;
        tb_json_error error = {0};
        size_t events = 0;
        TB_TEST_CONTEXT("trial %zu", trial);
        generate(&text);
        text.text[text.length] = '\0';
        TB_TEST_REQUIRE_UINT(read_all(text.text, NULL, &events, &error), ==, TB_JSON_OK);
        TB_TEST_REQUIRE_UINT(events, ==, text.events);
        TB_TEST_REQUIRE_UINT(tb_json_parse(text.text, text.length, NULL, arena, &root, &error), ==,
                             TB_JSON_OK);
        TB_TEST_REQUIRE_UINT(count_tree_events(root), ==, text.events);
        tb_arena_destroy(arena);
    }
}

/* Every prefix of a valid document is an error, never a crash. */
TB_TEST(serialize_json, every_truncation_is_an_error)
{
    static const char *const documents[] = {
        "{\"a\": [1, 2.5e-3, true, null, \"x\\u00e9\\uD83D\\uDE00\"], \"b\": {\"c\": false}}",
        "[[[[[\"deep\"]]]], -0, 123456789012345678901234567890]", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\""};
    for (size_t row = 0; row < TB_TEST_COUNT_OF(documents); row++)
    {
        const size_t length = tb_str_length(documents[row]);
        tb_arena *const arena = tb_arena_create(0);
        for (size_t prefix = 0; prefix < length; prefix++)
        {
            const tb_json_value *root = NULL;
            tb_json_error error = {0};
            TB_TEST_CONTEXT("document %zu, prefix %zu", row, prefix);
            TB_TEST_REQUIRE_UINT(tb_json_parse(documents[row], prefix, NULL, arena, &root, &error),
                                 ==, TB_JSON_MALFORMED);
            TB_TEST_REQUIRE_UINT(error.offset, <=, prefix);
        }
        tb_arena_destroy(arena);
    }
}

/* Random bytes changed in valid documents: any result, never a fault, which
 * the sanitizer builds of this test check. */
TB_TEST(serialize_json, mutated_documents_are_survived)
{
    static generated text;
    static const char replacements[] = "[]{}\",:\\u0e-+.9 \x00\x80\xFF/*tfn";
    for (size_t trial = 0; trial < 300; trial++)
    {
        tb_arena *const arena = tb_arena_create(0);
        tb_json_options options = {0};
        options.flags = (unsigned)tb_test_random_below(8);
        generate(&text);
        for (size_t change = 0; change < 1 + tb_test_random_below(4); change++)
        {
            const size_t at = (size_t)tb_test_random_below(text.length);
            text.text[at] = replacements[tb_test_random_below(sizeof(replacements) - 1)];
        }
        {
            const tb_json_value *root = NULL;
            tb_json_error error = {0};
            const tb_json_result result =
                tb_json_parse(text.text, text.length, &options, arena, &root, &error);
            TB_TEST_CONTEXT("trial %zu", trial);
            TB_TEST_REQUIRE((result == TB_JSON_OK) == (root != NULL));
            TB_TEST_REQUIRE_UINT(error.offset, <=, text.length);
        }
        tb_arena_destroy(arena);
    }
}

static void json_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    static unsigned char memory[4096];
    tb_arena *const arena = tb_arena_create_fixed(memory, sizeof(memory));
    const tb_json_value *root = NULL;
    const char *key = NULL;
    size_t length = 0;
    (void)tb_json_parse("[1]", 3, NULL, arena, &root, NULL);
    switch (choice)
    {
    case 0:
        (void)tb_json_at(root, 1);
        break;
    case 1:
        (void)tb_json_member_at(root, 0, &key, &length);
        break;
    case 2:
        (void)tb_json_parse(NULL, 1, NULL, arena, &root, NULL);
        break;
    case 3:
        (void)tb_json_parse("1", 1, NULL, NULL, &root, NULL);
        break;
    case 4:
        (void)tb_json_reader_create(NULL, 2, NULL, NULL);
        break;
    default:
        (void)tb_json_type_of(NULL);
        break;
    }
    tb_arena_destroy(arena);
}

TB_TEST(serialize_json, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 6; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(json_contract, &choice, "assertion failed");
    }
}
