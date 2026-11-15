/* The string functions are small, and their mistakes are off-by-one errors at
 * the ends of a range. Each search is therefore compared with a version
 * written the obvious way, for every range of a set of strings and for random
 * strings over a small alphabet, where matches and near-matches are common.
 * The functions that build strings are checked for exact contents and for an
 * exact allocation. */
#include "badger/str.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

/* --- the obvious versions ----------------------------------------------------- */

static bool is_in_set(char byte, const char *set)
{
    for (size_t index = 0; set[index] != '\0'; index++)
    {
        if (set[index] == byte)
        {
            return true;
        }
    }
    return false;
}

static bool occurs_at(const char *string, size_t position, size_t end, const char *pattern)
{
    const size_t pattern_length = strlen(pattern);

    if (pattern_length > end - position)
    {
        return false;
    }
    return memcmp(string + position, pattern, pattern_length) == 0;
}

static size_t naive_find_char(const char *string, size_t begin, size_t end, char character)
{
    for (size_t index = begin; index < end; index++)
    {
        if (string[index] == character)
        {
            return index;
        }
    }
    return TB_STR_NOT_FOUND;
}

static size_t naive_rfind_char(const char *string, size_t begin, size_t end, char character)
{
    size_t found = TB_STR_NOT_FOUND;

    for (size_t index = begin; index < end; index++)
    {
        if (string[index] == character)
        {
            found = index;
        }
    }
    return found;
}

static size_t naive_find_any(const char *string, size_t begin, size_t end, const char *set)
{
    for (size_t index = begin; index < end; index++)
    {
        if (is_in_set(string[index], set))
        {
            return index;
        }
    }
    return TB_STR_NOT_FOUND;
}

static size_t naive_rfind_any(const char *string, size_t begin, size_t end, const char *set)
{
    size_t found = TB_STR_NOT_FOUND;

    for (size_t index = begin; index < end; index++)
    {
        if (is_in_set(string[index], set))
        {
            found = index;
        }
    }
    return found;
}

static size_t naive_find(const char *string, size_t begin, size_t end, const char *pattern)
{
    for (size_t index = begin; index <= end; index++)
    {
        if (occurs_at(string, index, end, pattern))
        {
            return index;
        }
    }
    return TB_STR_NOT_FOUND;
}

static size_t naive_rfind(const char *string, size_t begin, size_t end, const char *pattern)
{
    size_t found = TB_STR_NOT_FOUND;

    for (size_t index = begin; index <= end; index++)
    {
        if (occurs_at(string, index, end, pattern))
        {
            found = index;
        }
    }
    return found;
}

static size_t naive_span(const char *string, size_t begin, size_t end, const char *set)
{
    size_t count = {0};

    while (begin + count < end && is_in_set(string[begin + count], set))
    {
        count++;
    }
    return count;
}

static size_t naive_rspan(const char *string, size_t begin, size_t end, const char *set)
{
    size_t count = {0};

    while (end - count > begin && is_in_set(string[end - count - 1], set))
    {
        count++;
    }
    return count;
}

/* Compares every search function with its obvious version on one range. */
static void check_searches(const char *string, size_t begin, size_t end, const char *needle)
{
    const char character = needle[0] == '\0' ? 'a' : needle[0];

    TB_TEST_CONTEXT("\"%s\" from %zu to %zu, looking for \"%s\"", string, begin, end, needle);
    TB_TEST_EXPECT_UINT(tb_str_find_char(string, begin, end, character), ==,
                        naive_find_char(string, begin, end, character));
    TB_TEST_EXPECT_UINT(tb_str_rfind_char(string, begin, end, character), ==,
                        naive_rfind_char(string, begin, end, character));
    TB_TEST_EXPECT_UINT(tb_str_find_any(string, begin, end, needle), ==,
                        naive_find_any(string, begin, end, needle));
    TB_TEST_EXPECT_UINT(tb_str_rfind_any(string, begin, end, needle), ==,
                        naive_rfind_any(string, begin, end, needle));
    TB_TEST_EXPECT_UINT(tb_str_find(string, begin, end, needle), ==,
                        naive_find(string, begin, end, needle));
    TB_TEST_EXPECT_UINT(tb_str_rfind(string, begin, end, needle), ==,
                        naive_rfind(string, begin, end, needle));
    TB_TEST_EXPECT_UINT(tb_str_span(string, begin, end, needle), ==,
                        naive_span(string, begin, end, needle));
    TB_TEST_EXPECT_UINT(tb_str_rspan(string, begin, end, needle), ==,
                        naive_rspan(string, begin, end, needle));
}

/* --- measuring and comparing -------------------------------------------------- */

TB_TEST(core_str, length_counts_bytes_up_to_the_terminator)
{
    TB_TEST_EXPECT_UINT(tb_str_length(""), ==, 0);
    TB_TEST_EXPECT_UINT(tb_str_length("a"), ==, 1);
    TB_TEST_EXPECT_UINT(tb_str_length("badger"), ==, 6);
    TB_TEST_EXPECT_UINT(tb_str_length("abc\0def"), ==, 3);
    /* Bytes, not characters: each of these letters takes two. */
    TB_TEST_EXPECT_UINT(tb_str_length("\xC3\xA9\xC3\xA8"), ==, 4);
}

TB_TEST(core_str, compare_orders_strings_by_unsigned_bytes)
{
    typedef struct compare_row
    {
        const char *smaller;
        const char *larger;
    } compare_row;
    static const compare_row rows[] = {
        {"", "a"},
        {"a", "b"},
        {"a", "aa"},
        {"abc", "abd"},
        {"ab", "abc"},
        {"B", "a"},
        /* A byte above 127 sorts after every ASCII byte, as an unsigned value. */
        {"\x7F", "\x80"},
        {"z", "\xC3\xA9"},
        /* UTF-8 keeps the order of code points: U+00E9 before U+65E5. */
        {"\xC3\xA9", "\xE6\x97\xA5"},
    };
    static const char *const same[] = {"", "a", "badger", "\xE6\x97\xA5"};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_INT(tb_str_compare(rows[index].smaller, rows[index].larger), <, 0);
        TB_TEST_EXPECT_INT(tb_str_compare(rows[index].larger, rows[index].smaller), >, 0);
        TB_TEST_EXPECT(!tb_str_equal(rows[index].smaller, rows[index].larger));
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(same); index++)
    {
        char copy[16] = {0};

        memcpy(copy, same[index], strlen(same[index]));
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_INT(tb_str_compare(same[index], copy), ==, 0);
        TB_TEST_EXPECT(tb_str_equal(same[index], copy));
    }
}

TB_TEST(core_str, prefixes_suffixes_and_matches_are_recognized)
{
    typedef struct match_row
    {
        const char *string;
        const char *pattern;
        bool starts;
        bool ends;
    } match_row;
    static const match_row rows[] = {
        {"", "", true, true},
        {"badger", "", true, true},
        {"badger", "badger", true, true},
        {"badger", "bad", true, false},
        {"badger", "ger", false, true},
        {"badger", "adg", false, false},
        {"badger", "badgers", false, false},
        {"", "a", false, false},
        {"aaa", "aa", true, true},
        {"image.png", ".png", false, true},
        {"image.png", "image", true, false},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const size_t length = strlen(rows[index].string);

        TB_TEST_CONTEXT("\"%s\" and \"%s\"", rows[index].string, rows[index].pattern);
        TB_TEST_EXPECT(tb_str_starts_with(rows[index].string, rows[index].pattern) ==
                       rows[index].starts);
        TB_TEST_EXPECT(tb_str_ends_with(rows[index].string, rows[index].pattern) ==
                       rows[index].ends);
        /* A match at a position is the same question, asked anywhere. */
        for (size_t position = 0; position <= length; position++)
        {
            TB_TEST_EXPECT(tb_str_matches_at(rows[index].string, position, rows[index].pattern) ==
                           occurs_at(rows[index].string, position, length, rows[index].pattern));
        }
    }
}

/* --- searching ---------------------------------------------------------------- */

static const char *const search_strings[] = {
    "",
    "a",
    "ab",
    "aaaa",
    "abcabc",
    "abababab",
    "hello, world",
    "a/b\\c/d",
    "  indented text  ",
    /* "cafe au lait" with an accented e, and three Japanese characters. */
    "caf\xC3\xA9 au lait",
    "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
    "\x80\xFF\x01\xFF\x80",
};

static const char *const needles[] = {
    "",    "a", "b",   "ab", "abc",      "aa",   "ba",           "l",        "lo",
    ", ",  "/", "/\\", " ",  "\xC3\xA9", "\xFF", "\xE6\x9C\xAC", "abababab", "hello, world!",
    "xyz",
};

TB_TEST(core_str, searches_agree_with_the_obvious_versions_on_every_range)
{
    for (size_t string_index = 0; string_index < TB_TEST_COUNT_OF(search_strings); string_index++)
    {
        const char *const string = search_strings[string_index];
        const size_t length = strlen(string);

        for (size_t needle_index = 0; needle_index < TB_TEST_COUNT_OF(needles); needle_index++)
        {
            for (size_t begin = 0; begin <= length; begin++)
            {
                for (size_t end = begin; end <= length; end++)
                {
                    check_searches(string, begin, end, needles[needle_index]);
                }
            }
        }
    }
}

TB_TEST(core_str, the_end_marker_means_the_end_of_the_string)
{
    for (size_t string_index = 0; string_index < TB_TEST_COUNT_OF(search_strings); string_index++)
    {
        const char *const string = search_strings[string_index];
        const size_t length = strlen(string);

        for (size_t needle_index = 0; needle_index < TB_TEST_COUNT_OF(needles); needle_index++)
        {
            const char *const needle = needles[needle_index];

            for (size_t begin = 0; begin <= length; begin++)
            {
                TB_TEST_CONTEXT("\"%s\" from %zu, looking for \"%s\"", string, begin, needle);
                TB_TEST_EXPECT_UINT(tb_str_find(string, begin, TB_STR_END, needle), ==,
                                    tb_str_find(string, begin, length, needle));
                TB_TEST_EXPECT_UINT(tb_str_rfind(string, begin, TB_STR_END, needle), ==,
                                    tb_str_rfind(string, begin, length, needle));
                TB_TEST_EXPECT_UINT(tb_str_rfind_any(string, begin, TB_STR_END, needle), ==,
                                    tb_str_rfind_any(string, begin, length, needle));
                TB_TEST_EXPECT_UINT(tb_str_rspan(string, begin, TB_STR_END, needle), ==,
                                    tb_str_rspan(string, begin, length, needle));
            }
        }
    }
}

TB_TEST(core_str, searches_find_what_a_caller_expects)
{
    static const char path[] = "assets/textures/stone.wall.png";
    static const char accented[] = "caf\xC3\xA9 au lait";

    /* A few results worked out by hand, as a check on the obvious versions. */
    TB_TEST_EXPECT_UINT(tb_str_find_char(path, 0, TB_STR_END, '/'), ==, 6);
    TB_TEST_EXPECT_UINT(tb_str_rfind_char(path, 0, TB_STR_END, '/'), ==, 15);
    TB_TEST_EXPECT_UINT(tb_str_rfind_char(path, 0, TB_STR_END, '.'), ==, 26);
    TB_TEST_EXPECT_UINT(tb_str_find_char(path, 7, TB_STR_END, '/'), ==, 15);
    TB_TEST_EXPECT_UINT(tb_str_find_char(path, 16, TB_STR_END, '/'), ==, TB_STR_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_str_find(path, 0, TB_STR_END, "textures"), ==, 7);
    TB_TEST_EXPECT_UINT(tb_str_find(path, 0, 14, "textures"), ==, TB_STR_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_str_find_any(path, 0, TB_STR_END, "./"), ==, 6);
    TB_TEST_EXPECT_UINT(tb_str_rfind_any(path, 0, TB_STR_END, "./"), ==, 26);
    TB_TEST_EXPECT_UINT(tb_str_span("  \t value", 0, TB_STR_END, " \t"), ==, 4);
    TB_TEST_EXPECT_UINT(tb_str_rspan("value \n", 0, TB_STR_END, " \n"), ==, 2);
    TB_TEST_EXPECT_UINT(tb_str_span("12345abc", 0, TB_STR_END, "0123456789"), ==, 5);

    /* The terminator is not part of the string. */
    TB_TEST_EXPECT_UINT(tb_str_find_char(path, 0, TB_STR_END, '\0'), ==, TB_STR_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_str_rfind_char(path, 0, TB_STR_END, '\0'), ==, TB_STR_NOT_FOUND);

    /* A multi-byte character is found at the index of its first byte, and
     * only where it really is. */
    TB_TEST_EXPECT_UINT(tb_str_find(accented, 0, TB_STR_END, "\xC3\xA9"), ==, 3);
    TB_TEST_EXPECT_UINT(tb_str_find(accented, 0, TB_STR_END, " au"), ==, 5);
    TB_TEST_EXPECT_UINT(tb_str_find(accented, 0, TB_STR_END, "\xC3\xA8"), ==, TB_STR_NOT_FOUND);
}

/* A random string over a small alphabet, so that patterns occur often and
 * near-matches more often still. */
static void fill_random_string(char *buffer, size_t length)
{
    static const char alphabet[] = "abc";

    for (size_t index = 0; index < length; index++)
    {
        buffer[index] = alphabet[tb_test_random_below(sizeof(alphabet) - 1)];
    }
    buffer[length] = '\0';
}

TB_TEST(core_str, searches_agree_with_the_obvious_versions_on_random_strings)
{
    enum
    {
        ITERATION_COUNT = 30000,
        STRING_LIMIT = 40,
        NEEDLE_LIMIT = 4
    };

    for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++)
    {
        char string[STRING_LIMIT + 1] = {0};
        char needle[NEEDLE_LIMIT + 1] = {0};
        const size_t length = (size_t)tb_test_random_below(STRING_LIMIT + 1);
        const size_t first = (size_t)tb_test_random_below(length + 1);
        const size_t second = (size_t)tb_test_random_below(length + 1);

        fill_random_string(string, length);
        fill_random_string(needle, (size_t)tb_test_random_below(NEEDLE_LIMIT + 1));
        check_searches(string, first < second ? first : second, first < second ? second : first,
                       needle);
    }
}

TB_TEST(core_str, a_long_string_is_searched_to_its_last_byte)
{
    enum
    {
        LENGTH = 100000
    };
    char *const haystack = tb_str_repeat(NULL, "ab", LENGTH / 2);
    char *const needle_at_end = tb_str_cat(NULL, haystack + LENGTH - 6, "");

    haystack[LENGTH - 1] = 'c';
    needle_at_end[5] = 'c';
    TB_TEST_EXPECT_UINT(tb_str_length(haystack), ==, LENGTH);
    TB_TEST_EXPECT_UINT(tb_str_find_char(haystack, 0, TB_STR_END, 'c'), ==, LENGTH - 1);
    TB_TEST_EXPECT_UINT(tb_str_rfind_char(haystack, 0, TB_STR_END, 'a'), ==, LENGTH - 2);
    /* Every candidate but the last is a near-match that fails on its last byte. */
    TB_TEST_EXPECT_UINT(tb_str_find(haystack, 0, TB_STR_END, needle_at_end), ==, LENGTH - 6);
    TB_TEST_EXPECT_UINT(tb_str_rfind(haystack, 0, TB_STR_END, "abab"), ==, LENGTH - 6);
    TB_TEST_EXPECT_UINT(tb_str_span(haystack, 0, TB_STR_END, "ab"), ==, LENGTH - 1);
    tb_str_free(NULL, needle_at_end);
    tb_str_free(NULL, haystack);
}

/* --- building ----------------------------------------------------------------- */

/* Checks a built string: its contents, and that its allocation is exactly its
 * bytes and its terminator. Then releases it. */
static void expect_built(tb_test_allocator *tracker, const tb_allocator *allocator, char *built,
                         const char *expected)
{
    TB_TEST_REQUIRE_POINTER(built, !=, NULL);
    TB_TEST_EXPECT_STRING(built, ==, expected);
    TB_TEST_EXPECT_UINT(tracker->live_block_count, ==, 1);
    TB_TEST_EXPECT_UINT(tracker->live_byte_count, ==, strlen(expected) + 1);
    TB_TEST_EXPECT(tb_test_allocator_owns(tracker, built, strlen(expected) + 1));
    tb_str_free(allocator, built);
    TB_TEST_EXPECT_UINT(tracker->live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker->first_violation, ==, "none");
}

TB_TEST(core_str, dup_and_sub_copy_a_string_or_any_part_of_it)
{
    static const char source[] = "hello, world";
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const size_t length = sizeof(source) - 1;

    expect_built(&tracker, &allocator, tb_str_dup(&allocator, source), source);
    expect_built(&tracker, &allocator, tb_str_dup(&allocator, ""), "");
    for (size_t begin = 0; begin <= length; begin++)
    {
        for (size_t end = begin; end <= length; end++)
        {
            char expected[sizeof(source)] = {0};

            memcpy(expected, source + begin, end - begin);
            TB_TEST_CONTEXT("from %zu to %zu", begin, end);
            expect_built(&tracker, &allocator, tb_str_sub(&allocator, source, begin, end),
                         expected);
        }
        expect_built(&tracker, &allocator, tb_str_sub(&allocator, source, begin, TB_STR_END),
                     source + begin);
    }
}

TB_TEST(core_str, cat_and_join_put_strings_end_to_end)
{
    static const char *const parts[] = {"assets", "textures", "", "stone.png"};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    expect_built(&tracker, &allocator, tb_str_cat(&allocator, "", ""), "");
    expect_built(&tracker, &allocator, tb_str_cat(&allocator, "bad", ""), "bad");
    expect_built(&tracker, &allocator, tb_str_cat(&allocator, "", "ger"), "ger");
    expect_built(&tracker, &allocator, tb_str_cat(&allocator, "bad", "ger"), "badger");

    expect_built(&tracker, &allocator, tb_str_join(&allocator, NULL, 0, "/"), "");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 0, "/"), "");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 1, "/"), "assets");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 2, "/"), "assets/textures");
    /* The separator goes between neighbors, an empty string included. */
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 4, "/"),
                 "assets/textures//stone.png");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 4, ", "),
                 "assets, textures, , stone.png");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 4, ""),
                 "assetstexturesstone.png");
    expect_built(&tracker, &allocator, tb_str_join(&allocator, parts, 4, NULL),
                 "assetstexturesstone.png");
}

TB_TEST(core_str, repeat_reverse_and_map_build_new_strings)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    char every_byte[256] = {0};
    char every_byte_but_a[256] = {0};

    /* Every byte value a string can hold, in order, and the same with the
     * one letter replaced. */
    for (size_t value = 1; value < 256; value++)
    {
        every_byte[value - 1] = (char)(unsigned char)value;
        every_byte_but_a[value - 1] = (char)(unsigned char)(value == 'a' ? 'b' : value);
    }

    expect_built(&tracker, &allocator, tb_str_repeat(&allocator, "ab", 0), "");
    expect_built(&tracker, &allocator, tb_str_repeat(&allocator, "ab", 1), "ab");
    expect_built(&tracker, &allocator, tb_str_repeat(&allocator, "ab", 3), "ababab");
    expect_built(&tracker, &allocator, tb_str_repeat(&allocator, "", 1000), "");
    expect_built(&tracker, &allocator, tb_str_repeat(&allocator, "-", 10), "----------");

    expect_built(&tracker, &allocator, tb_str_reverse(&allocator, ""), "");
    expect_built(&tracker, &allocator, tb_str_reverse(&allocator, "a"), "a");
    expect_built(&tracker, &allocator, tb_str_reverse(&allocator, "ab"), "ba");
    expect_built(&tracker, &allocator, tb_str_reverse(&allocator, "badger"), "regdab");

    /* No bytes to replace: a copy. */
    expect_built(&tracker, &allocator, tb_str_map(&allocator, "badger", "", ""), "badger");
    expect_built(&tracker, &allocator, tb_str_map(&allocator, "a\\b\\c", "\\", "/"), "a/b/c");
    expect_built(&tracker, &allocator,
                 tb_str_map(&allocator, "Tenacious Badger", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                            "abcdefghijklmnopqrstuvwxyz"),
                 "tenacious badger");
    /* Each byte is replaced once: b does not become c by way of a. */
    expect_built(&tracker, &allocator, tb_str_map(&allocator, "abc", "ab", "bc"), "bcc");
    /* A byte listed twice takes its last counterpart. */
    expect_built(&tracker, &allocator, tb_str_map(&allocator, "aaa", "aa", "xy"), "yyy");
    /* A byte that is not listed stays as it is, whatever its value. */
    expect_built(&tracker, &allocator, tb_str_map(&allocator, every_byte, "a", "b"),
                 every_byte_but_a);
    /* Bytes above 127 are mapped like any other. */
    expect_built(&tracker, &allocator, tb_str_map(&allocator, "\xFF-\x80", "\xFF\x80", "AB"),
                 "A-B");
}

TB_TEST(core_str, strings_can_be_built_on_the_heap_and_in_an_arena)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator in_arena = tb_arena_allocator(arena);
    char *const on_heap = tb_str_cat(NULL, "heap", "ed");
    char *in_the_arena = NULL;

    TB_TEST_EXPECT_STRING(on_heap, ==, "heaped");
    tb_str_free(NULL, on_heap);
    tb_str_free(NULL, NULL);

    /* Strings in an arena are released with it, not one by one. */
    for (size_t index = 0; index < 1000; index++)
    {
        in_the_arena = tb_str_repeat(&in_arena, "badger ", index % 7);
    }
    TB_TEST_EXPECT_STRING(in_the_arena, ==, "badger badger badger badger badger ");
    tb_arena_destroy(arena);
}

/* --- running out of memory ---------------------------------------------------- */

static void repeat_beyond_memory(void *context)
{
    (void)tb_str_repeat(context, "ab", SIZE_MAX);
}

static void dup_a_string(void *context)
{
    (void)tb_str_dup(context, "badger");
}

static void join_two_strings(void *context)
{
    static const char *const parts[] = {"bad", "ger"};

    (void)tb_str_join(context, parts, 2, "-");
}

static void map_a_string(void *context)
{
    (void)tb_str_map(context, "badger", "a", "e");
}

TB_TEST(core_str, a_string_that_cannot_be_allocated_ends_the_process)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);

    /* A length that overflows is refused before the allocator is asked. */
    TB_TEST_EXPECT_TRAP(repeat_beyond_memory, &allocator, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, 0);

    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(dup_a_string, &allocator, "out of memory: 1 x 7 bytes");
    TB_TEST_EXPECT_TRAP(join_two_strings, &allocator, "out of memory: 1 x 8 bytes");
    TB_TEST_EXPECT_TRAP(map_a_string, &allocator, "out of memory: 1 x 7 bytes");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

typedef struct range_request
{
    const char *string;
    size_t begin;
    size_t end;
} range_request;

static void find_char_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_find_char(request->string, request->begin, request->end, 'a');
}

static void rfind_char_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_rfind_char(request->string, request->begin, request->end, 'a');
}

static void find_any_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_find_any(request->string, request->begin, request->end, "a");
}

static void rfind_any_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_rfind_any(request->string, request->begin, request->end, "a");
}

static void find_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_find(request->string, request->begin, request->end, "a");
}

static void rfind_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_rfind(request->string, request->begin, request->end, "a");
}

static void span_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_span(request->string, request->begin, request->end, "a");
}

static void rspan_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_rspan(request->string, request->begin, request->end, "a");
}

static void sub_of_range(void *context)
{
    const range_request *const request = context;

    (void)tb_str_sub(NULL, request->string, request->begin, request->end);
}

static tb_test_trap_body *const range_bodies[] = {
    find_char_in_range, rfind_char_in_range, find_any_in_range, rfind_any_in_range, find_in_range,
    rfind_in_range,     span_in_range,       rspan_in_range,    sub_of_range,
};

TB_TEST(core_str, a_range_outside_the_string_violates_the_contract)
{
    static const range_request requests[] = {
        /* Past the end, backward, and starting past the end. */
        {"badger", 0, 7},
        {"badger", 4, 3},
        {"badger", 7, TB_STR_END},
        {"", 0, 1},
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(requests); row++)
    {
        range_request request = requests[row];

        for (size_t index = 0; index < TB_TEST_COUNT_OF(range_bodies); index++)
        {
            TB_TEST_CONTEXT("request %zu, function %zu", row, index);
            TB_TEST_EXPECT_TRAP(range_bodies[index], &request,
                                "the range must be in order and within the string");
        }
    }
}

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_str_length(NULL);
}

static void compare_null_left(void *context)
{
    (void)context;
    (void)tb_str_compare(NULL, "a");
}

static void compare_null_right(void *context)
{
    (void)context;
    (void)tb_str_equal("a", NULL);
}

static void starts_with_null(void *context)
{
    (void)context;
    (void)tb_str_starts_with("a", NULL);
}

static void ends_with_null(void *context)
{
    (void)context;
    (void)tb_str_ends_with("a", NULL);
}

static void ends_null(void *context)
{
    (void)context;
    (void)tb_str_ends_with(NULL, "a");
}

static void matches_null(void *context)
{
    (void)context;
    (void)tb_str_matches_at(NULL, 0, "a");
}

static void matches_past_the_end(void *context)
{
    (void)context;
    (void)tb_str_matches_at("abc", 4, "");
}

static void find_any_of_null(void *context)
{
    (void)context;
    (void)tb_str_find_any("abc", 0, TB_STR_END, NULL);
}

static void find_null_pattern(void *context)
{
    (void)context;
    (void)tb_str_find("abc", 0, TB_STR_END, NULL);
}

static void span_null_set(void *context)
{
    (void)context;
    (void)tb_str_span("abc", 0, TB_STR_END, NULL);
}

static void cat_null(void *context)
{
    (void)context;
    (void)tb_str_cat(NULL, "a", NULL);
}

static void join_without_strings(void *context)
{
    (void)context;
    (void)tb_str_join(NULL, NULL, 2, "/");
}

static void join_a_missing_string(void *context)
{
    static const char *const parts[] = {"a", NULL, "c"};

    (void)context;
    (void)tb_str_join(NULL, parts, 3, "/");
}

static void repeat_null(void *context)
{
    (void)context;
    (void)tb_str_repeat(NULL, NULL, 2);
}

static void reverse_null(void *context)
{
    (void)context;
    (void)tb_str_reverse(NULL, NULL);
}

static void map_with_unequal_sets(void *context)
{
    (void)context;
    (void)tb_str_map(NULL, "abc", "ab", "x");
}

static void map_null(void *context)
{
    (void)context;
    (void)tb_str_map(NULL, NULL, "a", "b");
}

TB_TEST(core_str, missing_strings_and_bad_arguments_violate_the_contract)
{
    range_request missing = {NULL, 0, TB_STR_END};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(range_bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(range_bodies[index], &missing, "string != NULL");
    }
    TB_TEST_EXPECT_TRAP(length_of_null, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(compare_null_left, NULL, "left != NULL");
    TB_TEST_EXPECT_TRAP(compare_null_right, NULL, "right != NULL");
    TB_TEST_EXPECT_TRAP(starts_with_null, NULL, "prefix != NULL");
    TB_TEST_EXPECT_TRAP(ends_with_null, NULL, "suffix != NULL");
    TB_TEST_EXPECT_TRAP(ends_null, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(matches_null, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(matches_past_the_end, NULL, "the position must be within the string");
    TB_TEST_EXPECT_TRAP(find_any_of_null, NULL, "set != NULL");
    TB_TEST_EXPECT_TRAP(find_null_pattern, NULL, "pattern != NULL");
    TB_TEST_EXPECT_TRAP(span_null_set, NULL, "set != NULL");
    TB_TEST_EXPECT_TRAP(cat_null, NULL, "right != NULL");
    TB_TEST_EXPECT_TRAP(join_without_strings, NULL, "strings != NULL || count == 0");
    TB_TEST_EXPECT_TRAP(join_a_missing_string, NULL, "every string to join must be present");
    TB_TEST_EXPECT_TRAP(repeat_null, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(reverse_null, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(map_with_unequal_sets, NULL, "from and to must have the same length");
    TB_TEST_EXPECT_TRAP(map_null, NULL, "string != NULL");
}
