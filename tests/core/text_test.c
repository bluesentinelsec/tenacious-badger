/* A text is a length and a pointer, so its promises are about what it points
 * to: a part of a text shares the bytes of the whole and copies nothing; a
 * text needs no terminator and may hold zero bytes; the empty text has no
 * bytes at all; and the functions that do allocate take exactly the bytes of
 * the result. The searches share their core with str.h, which checks them
 * against the obvious versions, so here they are checked against str.h. */
#include "badger/text.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

/* Checks that a text has exactly the bytes of a C string. */
static void expect_text(tb_text text, const char *expected)
{
    TB_TEST_EXPECT_UINT(text.length, ==, strlen(expected));
    if (text.length == strlen(expected) && text.length > 0)
    {
        TB_TEST_EXPECT_BYTES(text.bytes, expected, text.length);
    }
}

/* Checks a text that owns its bytes: its contents, and that its allocation is
 * exactly those bytes. Then releases it. */
static void expect_built(tb_test_allocator *tracker, const tb_allocator *allocator, tb_text built,
                         const char *expected)
{
    const size_t length = strlen(expected);

    expect_text(built, expected);
    TB_TEST_EXPECT_UINT(tracker->live_byte_count, ==, length);
    if (length == 0)
    {
        /* An empty result allocates nothing and has no bytes. */
        TB_TEST_EXPECT_UINT(tracker->live_block_count, ==, 0);
        TB_TEST_EXPECT_POINTER(built.bytes, ==, NULL);
    }
    else
    {
        TB_TEST_EXPECT_UINT(tracker->live_block_count, ==, 1);
        TB_TEST_EXPECT(tb_test_allocator_owns(tracker, built.bytes, length));
    }
    tb_text_free(allocator, built);
    TB_TEST_EXPECT_UINT(tracker->live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker->first_violation, ==, "none");
}

/* --- texts that borrow -------------------------------------------------------- */

TB_TEST(core_text, a_text_is_a_length_and_a_pointer_to_bytes_it_does_not_copy)
{
    static const char source[] = "tenacious badger";
    static const tb_text constant = TB_TEXT_INIT("badger");
    const tb_text from_string = tb_text_from_cstr(source);
    const tb_text from_bytes = tb_text_make(source + 10, 6);
    const tb_text from_literal = TB_TEXT("badger");
    const tb_text empty = {0};

    TB_TEST_EXPECT_POINTER(from_string.bytes, ==, source);
    TB_TEST_EXPECT_UINT(from_string.length, ==, 16);
    TB_TEST_EXPECT_POINTER(from_bytes.bytes, ==, source + 10);
    expect_text(from_bytes, "badger");
    expect_text(from_literal, "badger");
    expect_text(constant, "badger");
    TB_TEST_EXPECT(tb_text_equal(from_bytes, from_literal));
    TB_TEST_EXPECT(tb_text_equal(from_bytes, constant));

    /* The empty text is the zero value, however it is made. */
    TB_TEST_EXPECT_UINT(empty.length, ==, 0);
    TB_TEST_EXPECT(tb_text_equal(empty, TB_TEXT("")));
    TB_TEST_EXPECT(tb_text_equal(empty, tb_text_from_cstr("")));
    TB_TEST_EXPECT(tb_text_equal(empty, tb_text_make(NULL, 0)));
    TB_TEST_EXPECT(tb_text_equal(empty, tb_text_make(source, 0)));
    TB_TEST_EXPECT(!tb_text_equal(empty, from_literal));
}

TB_TEST(core_text, the_predefined_sets_hold_what_their_names_say)
{
    expect_text(TB_TEXT_DIGITS, "0123456789");
    expect_text(TB_TEXT_UPPERCASE, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    expect_text(TB_TEXT_LOWERCASE, "abcdefghijklmnopqrstuvwxyz");
    expect_text(TB_TEXT_WHITESPACE, " \t\n\r\f\v");
    TB_TEST_EXPECT_UINT(TB_TEXT_UPPERCASE.length, ==, TB_TEXT_LOWERCASE.length);
}

TB_TEST(core_text, a_part_of_a_text_shares_its_bytes)
{
    static const char source[] = "hello, world";
    const tb_text whole = tb_text_from_cstr(source);
    const size_t blocks = tb_memory_allocation_count();

    for (size_t begin = 0; begin <= whole.length; begin++)
    {
        for (size_t end = begin; end <= whole.length; end++)
        {
            const tb_text part = tb_text_sub(whole, begin, end);

            TB_TEST_CONTEXT("from %zu to %zu", begin, end);
            TB_TEST_EXPECT_UINT(part.length, ==, end - begin);
            /* An empty part still says where it is. */
            TB_TEST_EXPECT_POINTER(part.bytes, ==, source + begin);
        }
        TB_TEST_EXPECT(tb_text_equal(tb_text_sub(whole, begin, TB_TEXT_END),
                                     tb_text_from_cstr(source + begin)));
    }
    /* A part of a part still points into the original. */
    TB_TEST_EXPECT_POINTER(tb_text_sub(tb_text_sub(whole, 7, TB_TEXT_END), 1, 3).bytes, ==,
                           source + 8);
    /* The empty text has no bytes, and neither has a part of it. */
    TB_TEST_EXPECT_POINTER(tb_text_sub(tb_text_make(NULL, 0), 0, TB_TEXT_END).bytes, ==, NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
}

TB_TEST(core_text, trim_drops_a_set_of_bytes_from_both_ends)
{
    typedef struct trim_row
    {
        const char *text;
        const char *set;
        const char *expected;
    } trim_row;
    static const trim_row rows[] = {
        {"", " ", ""},
        {"badger", " ", "badger"},
        {"  badger  ", " ", "badger"},
        {"  badger", " ", "badger"},
        {"badger  ", " ", "badger"},
        {" \t badger \r\n", " \t\r\n", "badger"},
        {"   ", " ", ""},
        {"a b", " ", "a b"},
        {"xxbadgerxx", "x", "badger"},
        {"badger", "", "badger"},
        {"//a/b//", "/", "a/b"},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const tb_text text = tb_text_from_cstr(rows[index].text);
        const tb_text trimmed = tb_text_trim(text, tb_text_from_cstr(rows[index].set));

        TB_TEST_CONTEXT("trimming \"%s\" of \"%s\"", rows[index].text, rows[index].set);
        expect_text(trimmed, rows[index].expected);
    }
    expect_text(tb_text_trim(TB_TEXT("  12 34\n"), TB_TEXT_WHITESPACE), "12 34");
}

/* --- comparing ---------------------------------------------------------------- */

static const char *const sample_strings[] = {
    "",       "a",    "ab",   "abc",         "abd",  "b",   "badger",    "badgers",
    "Badger", "\x7F", "\x80", "caf\xC3\xA9", "aaaa", "aba", "image.png",
};

TB_TEST(core_text, comparisons_agree_with_the_c_string_functions)
{
    for (size_t left = 0; left < TB_TEST_COUNT_OF(sample_strings); left++)
    {
        for (size_t right = 0; right < TB_TEST_COUNT_OF(sample_strings); right++)
        {
            const char *const left_string = sample_strings[left];
            const char *const right_string = sample_strings[right];
            const tb_text left_text = tb_text_from_cstr(left_string);
            const tb_text right_text = tb_text_from_cstr(right_string);
            const int expected = tb_str_compare(left_string, right_string);
            const int actual = tb_text_compare(left_text, right_text);

            TB_TEST_CONTEXT("\"%s\" and \"%s\"", left_string, right_string);
            TB_TEST_EXPECT((actual < 0) == (expected < 0));
            TB_TEST_EXPECT((actual > 0) == (expected > 0));
            TB_TEST_EXPECT(tb_text_equal(left_text, right_text) == (expected == 0));
            TB_TEST_EXPECT(tb_text_starts_with(left_text, right_text) ==
                           tb_str_starts_with(left_string, right_string));
            TB_TEST_EXPECT(tb_text_ends_with(left_text, right_text) ==
                           tb_str_ends_with(left_string, right_string));
            for (size_t position = 0; position <= left_text.length; position++)
            {
                TB_TEST_EXPECT(tb_text_matches_at(left_text, position, right_text) ==
                               tb_str_matches_at(left_string, position, right_string));
            }
        }
    }
}

TB_TEST(core_text, a_zero_byte_is_a_byte_like_any_other)
{
    static const char with_zeros[] = {'a', '\0', 'b', '\0', 'c'};
    const tb_text text = tb_text_make(with_zeros, sizeof(with_zeros));
    const tb_text shorter = tb_text_make(with_zeros, 3);
    const tb_text zero = tb_text_make(with_zeros + 1, 1);

    TB_TEST_EXPECT_UINT(text.length, ==, 5);
    TB_TEST_EXPECT_UINT(tb_text_find_char(text, 0, TB_TEXT_END, '\0'), ==, 1);
    TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, 0, TB_TEXT_END, '\0'), ==, 3);
    TB_TEST_EXPECT_UINT(tb_text_find_char(text, 0, TB_TEXT_END, 'c'), ==, 4);
    TB_TEST_EXPECT_UINT(tb_text_find(text, 0, TB_TEXT_END, tb_text_make(with_zeros + 3, 2)), ==, 3);
    TB_TEST_EXPECT_UINT(tb_text_find_any(text, 0, TB_TEXT_END, zero), ==, 1);
    TB_TEST_EXPECT_UINT(tb_text_span(text, 1, TB_TEXT_END, zero), ==, 1);
    /* The bytes after a zero still count in a comparison. */
    TB_TEST_EXPECT_INT(tb_text_compare(shorter, text), <, 0);
    TB_TEST_EXPECT(!tb_text_equal(shorter, text));
    TB_TEST_EXPECT(tb_text_starts_with(text, shorter));
    TB_TEST_EXPECT(tb_text_ends_with(text, tb_text_make(with_zeros + 3, 2)));
}

/* --- searching ---------------------------------------------------------------- */

static const char *const search_strings[] = {
    "", "a", "aaaa", "abcabc", "abababab", "hello, world", "a/b\\c/d", "caf\xC3\xA9 au lait",
};

static const char *const needles[] = {
    "", "a", "b", "ab", "abc", "ba", "lo", "/", "/\\", "\xC3\xA9", "xyz", "hello, world!",
};

TB_TEST(core_text, searches_agree_with_the_c_string_functions_on_every_range)
{
    for (size_t string_index = 0; string_index < TB_TEST_COUNT_OF(search_strings); string_index++)
    {
        const char *const string = search_strings[string_index];
        const tb_text text = tb_text_from_cstr(string);

        for (size_t needle_index = 0; needle_index < TB_TEST_COUNT_OF(needles); needle_index++)
        {
            const char *const needle = needles[needle_index];
            const tb_text pattern = tb_text_from_cstr(needle);
            const char character = needle[0] == '\0' ? 'a' : needle[0];

            for (size_t begin = 0; begin <= text.length; begin++)
            {
                for (size_t end = begin; end <= text.length; end++)
                {
                    TB_TEST_CONTEXT("\"%s\" from %zu to %zu, looking for \"%s\"", string, begin,
                                    end, needle);
                    TB_TEST_EXPECT_UINT(tb_text_find_char(text, begin, end, character), ==,
                                        tb_str_find_char(string, begin, end, character));
                    TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, begin, end, character), ==,
                                        tb_str_rfind_char(string, begin, end, character));
                    TB_TEST_EXPECT_UINT(tb_text_find_any(text, begin, end, pattern), ==,
                                        tb_str_find_any(string, begin, end, needle));
                    TB_TEST_EXPECT_UINT(tb_text_rfind_any(text, begin, end, pattern), ==,
                                        tb_str_rfind_any(string, begin, end, needle));
                    TB_TEST_EXPECT_UINT(tb_text_find(text, begin, end, pattern), ==,
                                        tb_str_find(string, begin, end, needle));
                    TB_TEST_EXPECT_UINT(tb_text_rfind(text, begin, end, pattern), ==,
                                        tb_str_rfind(string, begin, end, needle));
                    TB_TEST_EXPECT_UINT(tb_text_span(text, begin, end, pattern), ==,
                                        tb_str_span(string, begin, end, needle));
                    TB_TEST_EXPECT_UINT(tb_text_rspan(text, begin, end, pattern), ==,
                                        tb_str_rspan(string, begin, end, needle));
                }
                TB_TEST_EXPECT_UINT(tb_text_rfind(text, begin, TB_TEXT_END, pattern), ==,
                                    tb_str_rfind(string, begin, TB_STR_END, needle));
            }
        }
    }
}

TB_TEST(core_text, a_search_stays_inside_a_text_that_is_part_of_a_longer_string)
{
    static const char source[] = "stone.png;wood.png;iron.png";
    /* "wood.png", with more text on both sides that must not be seen. */
    const tb_text middle = tb_text_make(source + 10, 8);
    const tb_text empty = {0};

    TB_TEST_EXPECT_UINT(tb_text_find_char(middle, 0, TB_TEXT_END, ';'), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_rfind_char(middle, 0, TB_TEXT_END, 's'), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find(middle, 0, TB_TEXT_END, TB_TEXT("png;")), ==,
                        TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find(middle, 0, TB_TEXT_END, TB_TEXT(".png")), ==, 4);
    TB_TEST_EXPECT_UINT(tb_text_span(middle, 0, TB_TEXT_END, TB_TEXT_LOWERCASE), ==, 4);
    TB_TEST_EXPECT_UINT(tb_text_rspan(middle, 0, TB_TEXT_END, TB_TEXT("png;iro")), ==, 3);
    TB_TEST_EXPECT(tb_text_ends_with(middle, TB_TEXT(".png")));
    TB_TEST_EXPECT(!tb_text_ends_with(middle, TB_TEXT(".png;")));
    /* A pattern that would only match by reading past the end does not. */
    TB_TEST_EXPECT(!tb_text_matches_at(middle, 4, TB_TEXT(".png;")));
    TB_TEST_EXPECT(tb_text_matches_at(middle, 4, TB_TEXT(".png")));

    /* The empty text, which has no bytes, can be searched and searched for. */
    TB_TEST_EXPECT_UINT(tb_text_find_char(empty, 0, TB_TEXT_END, 'a'), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find(empty, 0, TB_TEXT_END, empty), ==, 0);
    TB_TEST_EXPECT_UINT(tb_text_rfind(empty, 0, 0, empty), ==, 0);
    TB_TEST_EXPECT_UINT(tb_text_find(empty, 0, 0, TB_TEXT("a")), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find_any(middle, 0, TB_TEXT_END, empty), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_span(empty, 0, 0, TB_TEXT_DIGITS), ==, 0);
    TB_TEST_EXPECT_INT(tb_text_compare(empty, empty), ==, 0);
    TB_TEST_EXPECT(tb_text_starts_with(middle, empty));
    TB_TEST_EXPECT(tb_text_ends_with(empty, empty));
}

/* --- searches against the obvious way of doing them ----------------------------
 *
 * The engine's searches take shortcuts: a word at a time, the rarest byte of a
 * pattern first, a rolling hash when that goes badly. These are the same
 * searches written one byte at a time, to compare them with. */

static bool plain_set_has(tb_text set, char byte)
{
    for (size_t index = 0; index < set.length; index++)
    {
        if (set.bytes[index] == byte)
        {
            return true;
        }
    }
    return false;
}

static bool plain_matches(tb_text text, size_t start, size_t end, tb_text pattern)
{
    if (pattern.length > end - start)
    {
        return false;
    }
    for (size_t index = 0; index < pattern.length; index++)
    {
        if (text.bytes[start + index] != pattern.bytes[index])
        {
            return false;
        }
    }
    return true;
}

static size_t plain_find(tb_text text, size_t begin, size_t end, tb_text pattern)
{
    for (size_t start = begin; start <= end; start++)
    {
        if (plain_matches(text, start, end, pattern))
        {
            return start;
        }
    }
    return TB_TEXT_NOT_FOUND;
}

static size_t plain_rfind(tb_text text, size_t begin, size_t end, tb_text pattern)
{
    for (size_t start = end + 1; start > begin; start--)
    {
        if (plain_matches(text, start - 1, end, pattern))
        {
            return start - 1;
        }
    }
    return TB_TEXT_NOT_FOUND;
}

static size_t plain_find_any(tb_text text, size_t begin, size_t end, tb_text set)
{
    for (size_t index = begin; index < end; index++)
    {
        if (plain_set_has(set, text.bytes[index]))
        {
            return index;
        }
    }
    return TB_TEXT_NOT_FOUND;
}

static size_t plain_rfind_any(tb_text text, size_t begin, size_t end, tb_text set)
{
    for (size_t index = end; index > begin; index--)
    {
        if (plain_set_has(set, text.bytes[index - 1]))
        {
            return index - 1;
        }
    }
    return TB_TEXT_NOT_FOUND;
}

static size_t plain_span(tb_text text, size_t begin, size_t end, tb_text set)
{
    size_t index = begin;

    while (index < end && plain_set_has(set, text.bytes[index]))
    {
        index++;
    }
    return index - begin;
}

static size_t plain_rspan(tb_text text, size_t begin, size_t end, tb_text set)
{
    size_t index = end;

    while (index > begin && plain_set_has(set, text.bytes[index - 1]))
    {
        index--;
    }
    return end - index;
}

/* Fills a buffer with bytes drawn from the first `alphabet_size` of `alphabet`.
 * A small alphabet makes texts in which everything nearly matches. */
static void fill_from_alphabet(char *buffer, size_t length, const char *alphabet,
                               size_t alphabet_size)
{
    for (size_t index = 0; index < length; index++)
    {
        buffer[index] = alphabet[tb_test_random_below(alphabet_size)];
    }
}

TB_TEST(core_text, random_searches_agree_with_searches_one_byte_at_a_time)
{
    enum
    {
        ROUND_COUNT = 4000,
        LONGEST_TEXT = 300,
        LONGEST_PATTERN = 40,
        LARGEST_SET = 6
    };
    /* Letters common and rare, a blank, a zero byte, and bytes above 127,
     * which are in the upper words of a set of bytes. */
    static const char alphabet[] = {'a', 'b', ' ', 'e', 'z', '\0', 'Q', '\x80', '\xC3', '\xFF'};
    static const size_t alphabet_sizes[] = {1, 2, 3, sizeof(alphabet)};

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        char bytes[LONGEST_TEXT] = {0};
        char pattern_bytes[LONGEST_PATTERN] = {0};
        char set_bytes[LARGEST_SET] = {0};
        const size_t alphabet_size =
            alphabet_sizes[tb_test_random_below(TB_TEST_COUNT_OF(alphabet_sizes))];
        const size_t length = (size_t)tb_test_random_below(LONGEST_TEXT + 1);
        const size_t begin = (size_t)tb_test_random_below(length + 1);
        const size_t end = begin + (size_t)tb_test_random_below(length - begin + 1);
        const tb_text text = tb_text_make(bytes, length);
        tb_text pattern = {0};
        tb_text set = {0};
        char byte = {0};

        fill_from_alphabet(bytes, length, alphabet, alphabet_size);
        if (tb_test_random_below(2) == 0 && end > begin)
        {
            /* A pattern that is in the range, wherever it starts and ends. */
            const size_t start = begin + (size_t)tb_test_random_below(end - begin);
            size_t pattern_length = (size_t)tb_test_random_below(end - start + 1);

            if (pattern_length > LONGEST_PATTERN)
            {
                pattern_length = LONGEST_PATTERN;
            }
            pattern = tb_text_make(bytes + start, pattern_length);
        }
        else
        {
            const size_t pattern_length = (size_t)tb_test_random_below(LONGEST_PATTERN + 1);

            fill_from_alphabet(pattern_bytes, pattern_length, alphabet, alphabet_size);
            pattern = tb_text_make(pattern_bytes, pattern_length);
        }
        set = tb_text_make(set_bytes, (size_t)tb_test_random_below(LARGEST_SET + 1));
        fill_from_alphabet(set_bytes, set.length, alphabet, sizeof(alphabet));
        byte = alphabet[tb_test_random_below(sizeof(alphabet))];

        TB_TEST_CONTEXT("round %zu: %zu bytes of %zu kinds, range %zu to %zu, pattern of %zu",
                        round, length, alphabet_size, begin, end, pattern.length);
        TB_TEST_EXPECT_UINT(tb_text_find(text, begin, end, pattern), ==,
                            plain_find(text, begin, end, pattern));
        TB_TEST_EXPECT_UINT(tb_text_rfind(text, begin, end, pattern), ==,
                            plain_rfind(text, begin, end, pattern));
        TB_TEST_EXPECT_UINT(tb_text_find_char(text, begin, end, byte), ==,
                            plain_find(text, begin, end, tb_text_make(&byte, 1)));
        TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, begin, end, byte), ==,
                            plain_rfind(text, begin, end, tb_text_make(&byte, 1)));
        TB_TEST_EXPECT_UINT(tb_text_find_any(text, begin, end, set), ==,
                            plain_find_any(text, begin, end, set));
        TB_TEST_EXPECT_UINT(tb_text_rfind_any(text, begin, end, set), ==,
                            plain_rfind_any(text, begin, end, set));
        TB_TEST_EXPECT_UINT(tb_text_span(text, begin, end, set), ==,
                            plain_span(text, begin, end, set));
        TB_TEST_EXPECT_UINT(tb_text_rspan(text, begin, end, set), ==,
                            plain_rspan(text, begin, end, set));
    }
}

/* A backward search for a byte reads eight bytes at a time. Every length up
 * to a few words, with the byte at every place and at none, makes it start,
 * stop, and finish on every boundary. */
TB_TEST(core_text, a_byte_is_found_from_the_end_wherever_it_is)
{
    enum
    {
        LONGEST = 40
    };

    for (size_t length = 0; length <= LONGEST; length++)
    {
        char bytes[LONGEST + 2] = {0};
        /* The text starts one byte into the buffer, after a byte that must
         * not be found. */
        const tb_text text = tb_text_make(bytes + 1, length);

        memset(bytes, '.', sizeof(bytes));
        bytes[0] = '#';
        bytes[length + 1] = '#';
        TB_TEST_CONTEXT("%zu bytes without the byte", length);
        TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, 0, TB_TEXT_END, '#'), ==, TB_TEXT_NOT_FOUND);
        for (size_t place = 0; place < length; place++)
        {
            bytes[1 + place] = '#';
            TB_TEST_CONTEXT("%zu bytes with the byte at %zu", length, place);
            TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, 0, TB_TEXT_END, '#'), ==, place);
            /* An earlier one as well: the later one is the one found. */
            bytes[1] = '#';
            TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, 0, TB_TEXT_END, '#'), ==, place);
            /* A range that ends before it finds the earlier one, or nothing. */
            TB_TEST_EXPECT_UINT(tb_text_rfind_char(text, 0, place, '#'), ==,
                                place == 0 ? TB_TEXT_NOT_FOUND : 0);
            bytes[1] = '.';
            bytes[1 + place] = '.';
        }
    }
}

/* Writes the text of `length` letters that a number spells: bit `index` of
 * the number chooses between 'a' and 'b' for byte `index`. */
static void spell(char *bytes, size_t length, size_t number)
{
    for (size_t index = 0; index < length; index++)
    {
        bytes[index] = ((number >> index) & 1u) != 0 ? 'b' : 'a';
    }
}

/* Every text of up to seven letters from two, every range of it, and every
 * pattern of up to three: small enough to try them all, and every way a
 * match can lie against the ends of a range is among them. */
TB_TEST(core_text, every_small_pattern_search_agrees_with_a_search_one_byte_at_a_time)
{
    enum
    {
        LONGEST_TEXT = 7,
        LONGEST_PATTERN = 3
    };

    for (size_t length = 0; length <= LONGEST_TEXT; length++)
    {
        for (size_t number = 0; number < ((size_t)1 << length); number++)
        {
            char bytes[LONGEST_TEXT + 1] = {0};
            const tb_text text = tb_text_make(bytes, length);

            spell(bytes, length, number);
            for (size_t pattern_length = 0; pattern_length <= LONGEST_PATTERN; pattern_length++)
            {
                for (size_t code = 0; code < ((size_t)1 << pattern_length); code++)
                {
                    char pattern_bytes[LONGEST_PATTERN + 1] = {0};
                    const tb_text pattern = tb_text_make(pattern_bytes, pattern_length);

                    spell(pattern_bytes, pattern_length, code);
                    for (size_t begin = 0; begin <= length; begin++)
                    {
                        for (size_t end = begin; end <= length; end++)
                        {
                            TB_TEST_CONTEXT("\"%s\" from %zu to %zu, looking for \"%s\"", bytes,
                                            begin, end, pattern_bytes);
                            TB_TEST_EXPECT_UINT(tb_text_find(text, begin, end, pattern), ==,
                                                plain_find(text, begin, end, pattern));
                            TB_TEST_EXPECT_UINT(tb_text_rfind(text, begin, end, pattern), ==,
                                                plain_rfind(text, begin, end, pattern));
                        }
                    }
                }
            }
        }
    }
}

/* When a search has turned to its rolling hash, a stretch of text with the
 * hash of the pattern is a candidate, not a match. These two words have the
 * same hash under it (multiply by 16,777,619 and add the next byte, in 32
 * bits), found by trying random words until two agreed. */
TB_TEST(core_text, a_stretch_with_the_hash_of_the_pattern_is_not_a_match)
{
    const tb_text pattern = TB_TEXT("lxvzrwmy");
    /* The run of 'z', the pattern's rarest letter, is one false candidate
     * after another, which is what turns the search to the rolling hash
     * before it reaches the word with the pattern's hash. */
    const tb_text without = TB_TEXT("zzzzzzzzzzzzzzzz wussuccl and no more");
    const tb_text with = TB_TEXT("zzzzzzzzzzzzzzzz wussuccl lxvzrwmy");

    TB_TEST_EXPECT_UINT(tb_text_find(without, 0, TB_TEXT_END, pattern), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find(with, 0, TB_TEXT_END, pattern), ==, 26);
    /* The other way round as well. */
    TB_TEST_EXPECT_UINT(tb_text_find(with, 0, TB_TEXT_END, TB_TEXT("wussuccl")), ==, 17);
}

/* Text in which every place nearly matches is what makes a search slow that
 * compares at every candidate: here the search gives that up for a rolling
 * hash. The answers must be the same either way. */
TB_TEST(core_text, a_pattern_is_found_among_near_misses)
{
    enum
    {
        RUN_LENGTH = 32,
        RUN_COUNT = 200,
        LENGTH = RUN_LENGTH * RUN_COUNT
    };
    static char bytes[LENGTH + RUN_LENGTH] = {0};
    char pattern_bytes[RUN_LENGTH] = {0};
    const tb_text pattern = tb_text_make(pattern_bytes, RUN_LENGTH);
    const tb_text misses = tb_text_make(bytes, LENGTH);
    const tb_text with_a_match = tb_text_make(bytes, LENGTH + RUN_LENGTH);

    /* Runs of 31 'a' and a 'b': the pattern of 32 'a' fits nowhere. */
    memset(bytes, 'a', sizeof(bytes));
    for (size_t run = 0; run < RUN_COUNT; run++)
    {
        bytes[run * RUN_LENGTH + RUN_LENGTH - 1] = 'b';
    }
    memset(pattern_bytes, 'a', sizeof(pattern_bytes));
    TB_TEST_EXPECT_UINT(tb_text_find(misses, 0, TB_TEXT_END, pattern), ==, TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_rfind(misses, 0, TB_TEXT_END, pattern), ==, TB_TEXT_NOT_FOUND);

    /* With 32 more 'a' after the last 'b', it fits there and only there, at
     * the very end. */
    TB_TEST_EXPECT_UINT(tb_text_find(with_a_match, 0, TB_TEXT_END, pattern), ==, LENGTH);
    TB_TEST_EXPECT_UINT(tb_text_find(with_a_match, 0, LENGTH + RUN_LENGTH - 1, pattern), ==,
                        TB_TEXT_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_text_find(with_a_match, 17, TB_TEXT_END, pattern), ==, LENGTH);

    /* A match in the middle is found though the search has given up its
     * first method long before it: the 'b' that ends run 100 becomes an 'a',
     * and the pattern fits from the start of that run. */
    bytes[100 * RUN_LENGTH + RUN_LENGTH - 1] = 'a';
    TB_TEST_EXPECT_UINT(tb_text_find(misses, 0, TB_TEXT_END, pattern), ==, 100 * RUN_LENGTH);
    TB_TEST_EXPECT_UINT(tb_text_find(misses, 100 * RUN_LENGTH + 1, TB_TEXT_END, pattern), ==,
                        100 * RUN_LENGTH + 1);
    bytes[100 * RUN_LENGTH + RUN_LENGTH - 1] = 'b';
}

/* --- texts with bytes of their own -------------------------------------------- */

TB_TEST(core_text, copy_and_to_cstr_give_a_text_a_life_of_its_own)
{
    static const char with_zero[] = {'a', '\0', 'b'};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    char source[] = "stone.png";
    const tb_text borrowed = tb_text_sub(tb_text_from_cstr(source), 0, 5);
    const tb_text copy = tb_text_copy(&allocator, borrowed);
    char *string = NULL;

    /* Changing the source changes what borrowed it, and not the copy. */
    source[0] = 'S';
    expect_text(borrowed, "Stone");
    TB_TEST_EXPECT_POINTER(copy.bytes, !=, source);
    expect_built(&tracker, &allocator, copy, "stone");
    expect_built(&tracker, &allocator, tb_text_copy(&allocator, TB_TEXT("")), "");

    string = tb_text_to_cstr(&allocator, borrowed);
    TB_TEST_EXPECT_STRING(string, ==, "Stone");
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 6);
    tb_str_free(&allocator, string);

    string = tb_text_to_cstr(&allocator, TB_TEXT(""));
    TB_TEST_EXPECT_STRING(string, ==, "");
    tb_str_free(&allocator, string);

    /* All of the bytes are copied, though a C string shows them only up to
     * the first zero. */
    string = tb_text_to_cstr(&allocator, tb_text_make(with_zero, sizeof(with_zero)));
    TB_TEST_EXPECT_STRING(string, ==, "a");
    TB_TEST_EXPECT_BYTES(string, with_zero, sizeof(with_zero));
    TB_TEST_EXPECT_INT(string[3], ==, '\0');
    tb_allocator_free(&allocator, string, sizeof(with_zero) + 1, 1);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_text, cat_and_join_put_texts_end_to_end)
{
    static const tb_text parts[] = {TB_TEXT_INIT("assets"), TB_TEXT_INIT("textures"),
                                    TB_TEXT_INIT(""), TB_TEXT_INIT("stone.png")};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const tb_text none = {0};

    expect_built(&tracker, &allocator, tb_text_cat(&allocator, none, none), "");
    expect_built(&tracker, &allocator, tb_text_cat(&allocator, TB_TEXT("bad"), none), "bad");
    expect_built(&tracker, &allocator, tb_text_cat(&allocator, none, TB_TEXT("ger")), "ger");
    expect_built(&tracker, &allocator, tb_text_cat(&allocator, TB_TEXT("bad"), TB_TEXT("ger")),
                 "badger");

    expect_built(&tracker, &allocator, tb_text_join(&allocator, NULL, 0, TB_TEXT("/")), "");
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts, 1, TB_TEXT("/")), "assets");
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts, 2, TB_TEXT("/")),
                 "assets/textures");
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts, 4, TB_TEXT("/")),
                 "assets/textures//stone.png");
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts, 4, TB_TEXT(", ")),
                 "assets, textures, , stone.png");
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts, 4, none),
                 "assetstexturesstone.png");
    /* Empty texts joined with nothing are still nothing. */
    expect_built(&tracker, &allocator, tb_text_join(&allocator, parts + 2, 1, TB_TEXT("/")), "");
}

TB_TEST(core_text, repeat_reverse_and_map_build_new_texts)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const tb_text none = {0};
    char every_byte[256] = {0};
    char every_byte_but_a[256] = {0};
    tb_text mapped = {0};

    /* Every byte value, zero included, and the same with one letter replaced. */
    for (size_t value = 0; value < 256; value++)
    {
        every_byte[value] = (char)(unsigned char)value;
        every_byte_but_a[value] = (char)(unsigned char)(value == 'a' ? 'b' : value);
    }
    /* A byte that is not listed stays as it is, whatever its value. */
    mapped = tb_text_map(&allocator, tb_text_make(every_byte, sizeof(every_byte)), TB_TEXT("a"),
                         TB_TEXT("b"));
    TB_TEST_EXPECT_UINT(mapped.length, ==, sizeof(every_byte));
    TB_TEST_EXPECT_BYTES(mapped.bytes, every_byte_but_a, sizeof(every_byte_but_a));
    tb_text_free(&allocator, mapped);

    expect_built(&tracker, &allocator, tb_text_repeat(&allocator, TB_TEXT("ab"), 0), "");
    expect_built(&tracker, &allocator, tb_text_repeat(&allocator, TB_TEXT("ab"), 1), "ab");
    expect_built(&tracker, &allocator, tb_text_repeat(&allocator, TB_TEXT("ab"), 3), "ababab");
    expect_built(&tracker, &allocator, tb_text_repeat(&allocator, none, 1000), "");

    expect_built(&tracker, &allocator, tb_text_reverse(&allocator, none), "");
    expect_built(&tracker, &allocator, tb_text_reverse(&allocator, TB_TEXT("a")), "a");
    expect_built(&tracker, &allocator, tb_text_reverse(&allocator, TB_TEXT("badger")), "regdab");

    expect_built(&tracker, &allocator, tb_text_map(&allocator, TB_TEXT("badger"), none, none),
                 "badger");
    expect_built(
        &tracker, &allocator,
        tb_text_map(&allocator, TB_TEXT("Tenacious Badger"), TB_TEXT_UPPERCASE, TB_TEXT_LOWERCASE),
        "tenacious badger");
    expect_built(&tracker, &allocator,
                 tb_text_map(&allocator, TB_TEXT("a\\b\\c"), TB_TEXT("\\"), TB_TEXT("/")), "a/b/c");
    /* Each byte is replaced once, and a byte listed twice takes its last
     * counterpart. */
    expect_built(&tracker, &allocator,
                 tb_text_map(&allocator, TB_TEXT("abc"), TB_TEXT("ab"), TB_TEXT("bc")), "bcc");
    expect_built(&tracker, &allocator,
                 tb_text_map(&allocator, TB_TEXT("aaa"), TB_TEXT("aa"), TB_TEXT("xy")), "yyy");
    expect_built(&tracker, &allocator, tb_text_map(&allocator, none, TB_TEXT("a"), TB_TEXT("b")),
                 "");
}

TB_TEST(core_text, texts_built_in_an_arena_are_released_with_it)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator in_arena = tb_arena_allocator(arena);
    const size_t blocks = tb_memory_allocation_count();
    tb_text path = TB_TEXT("assets");

    for (size_t index = 0; index < 100; index++)
    {
        path = tb_text_cat(&in_arena, path, TB_TEXT("/x"));
    }
    TB_TEST_EXPECT_UINT(path.length, ==, 206);
    TB_TEST_EXPECT(tb_text_starts_with(path, TB_TEXT("assets/x/x")));
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    /* Releasing an empty text is always allowed. */
    tb_text_free(NULL, tb_text_make(NULL, 0));
    tb_arena_destroy(arena);
}

/* --- formatting --------------------------------------------------------------- */

TB_TEST(core_text, a_text_is_formatted_like_a_string_once_registered)
{
    static const char source[] = "stone.png;wood.png";
    const tb_text name = tb_text_make(source, 9);
    const tb_text empty = {0};
    char buffer[64] = {0};

    tb_text_register_format();
    /* Registering again is harmless. */
    tb_text_register_format();
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[stone.png]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%12T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[   stone.png]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%-12T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[stone.png   ]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%.5T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[stone]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%8.5T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[   stone]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%.50T]", name);
    TB_TEST_EXPECT_STRING(buffer, ==, "[stone.png]");
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "[%T][%3T]", empty, empty);
    TB_TEST_EXPECT_STRING(buffer, ==, "[][   ]");
    /* A text is one argument among others, taken by value. */
    (void)tb_fmt_buffer(buffer, sizeof(buffer), "%d %T %s %T", 1, name, "and", TB_TEXT("more"));
    TB_TEST_EXPECT_STRING(buffer, ==, "1 stone.png and more");
    (void)tb_fmt_register('T', NULL);
}

/* --- running out of memory ---------------------------------------------------- */

static void copy_a_text(void *context)
{
    (void)tb_text_copy(context, TB_TEXT("badger"));
}

static void join_two_texts(void *context)
{
    static const tb_text parts[] = {TB_TEXT_INIT("bad"), TB_TEXT_INIT("ger")};

    (void)tb_text_join(context, parts, 2, TB_TEXT("-"));
}

static void make_a_c_string(void *context)
{
    (void)tb_text_to_cstr(context, TB_TEXT("badger"));
}

static void repeat_beyond_memory(void *context)
{
    (void)tb_text_repeat(context, TB_TEXT("ab"), SIZE_MAX);
}

TB_TEST(core_text, a_text_that_cannot_be_allocated_ends_the_process)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);

    /* A length that overflows is refused before the allocator is asked. */
    TB_TEST_EXPECT_TRAP(repeat_beyond_memory, &allocator, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, 0);

    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(copy_a_text, &allocator, "out of memory: 1 x 6 bytes");
    TB_TEST_EXPECT_TRAP(join_two_texts, &allocator, "out of memory: 1 x 7 bytes");
    TB_TEST_EXPECT_TRAP(make_a_c_string, &allocator, "out of memory: 1 x 7 bytes");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

typedef struct range_request
{
    tb_text text;
    size_t begin;
    size_t end;
} range_request;

static void sub_of_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_sub(request->text, request->begin, request->end);
}

static void find_char_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_find_char(request->text, request->begin, request->end, 'a');
}

static void rfind_char_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_rfind_char(request->text, request->begin, request->end, 'a');
}

static void find_any_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_find_any(request->text, request->begin, request->end, TB_TEXT("a"));
}

static void rfind_any_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_rfind_any(request->text, request->begin, request->end, TB_TEXT("a"));
}

static void find_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_find(request->text, request->begin, request->end, TB_TEXT("a"));
}

static void rfind_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_rfind(request->text, request->begin, request->end, TB_TEXT("a"));
}

static void span_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_span(request->text, request->begin, request->end, TB_TEXT("a"));
}

static void rspan_in_range(void *context)
{
    const range_request *const request = context;

    (void)tb_text_rspan(request->text, request->begin, request->end, TB_TEXT("a"));
}

static tb_test_trap_body *const range_bodies[] = {
    sub_of_range,  find_char_in_range, rfind_char_in_range, find_any_in_range, rfind_any_in_range,
    find_in_range, rfind_in_range,     span_in_range,       rspan_in_range,
};

TB_TEST(core_text, a_range_outside_the_text_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(range_bodies); index++)
    {
        /* Past the end, backward, and starting past the end. */
        range_request past_the_end = {.text = TB_TEXT("badger"), .begin = 0, .end = 7};
        range_request backward = {.text = TB_TEXT("badger"), .begin = 4, .end = 3};
        range_request late_start = {.text = TB_TEXT("badger"), .begin = 7, .end = TB_TEXT_END};
        range_request without_bytes = {.text = {.length = 3, .bytes = NULL}, .end = 1};

        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(range_bodies[index], &past_the_end,
                            "the range must be in order and within the text");
        TB_TEST_EXPECT_TRAP(range_bodies[index], &backward,
                            "the range must be in order and within the text");
        TB_TEST_EXPECT_TRAP(range_bodies[index], &late_start,
                            "the range must be in order and within the text");
        TB_TEST_EXPECT_TRAP(range_bodies[index], &without_bytes,
                            "a text with a length must have bytes");
    }
}

static void make_from_null_string(void *context)
{
    (void)context;
    (void)tb_text_from_cstr(NULL);
}

static void make_with_a_length_and_no_bytes(void *context)
{
    (void)context;
    (void)tb_text_make(NULL, 4);
}

static void match_past_the_end(void *context)
{
    (void)context;
    (void)tb_text_matches_at(TB_TEXT("abc"), 4, TB_TEXT(""));
}

static void map_with_unequal_sets(void *context)
{
    (void)context;
    (void)tb_text_map(NULL, TB_TEXT("abc"), TB_TEXT("ab"), TB_TEXT("x"));
}

static void join_without_texts(void *context)
{
    (void)context;
    (void)tb_text_join(NULL, NULL, 2, TB_TEXT("/"));
}

TB_TEST(core_text, malformed_texts_and_bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(make_from_null_string, NULL, "string != NULL");
    TB_TEST_EXPECT_TRAP(make_with_a_length_and_no_bytes, NULL,
                        "a text with a length must have bytes");
    TB_TEST_EXPECT_TRAP(match_past_the_end, NULL, "the position must be within the text");
    TB_TEST_EXPECT_TRAP(map_with_unequal_sets, NULL, "from and to must have the same length");
    TB_TEST_EXPECT_TRAP(join_without_texts, NULL, "texts != NULL || count == 0");
}
