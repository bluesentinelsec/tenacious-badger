/* Filter patterns decide which cases run. A wrong match silently runs too few
 * tests, so the matcher is checked against a table of edge cases and against
 * an independent reference implementation on random inputs. */
#include "internal.h"

#include <string.h>

typedef struct pattern_row
{
    const char *pattern;
    const char *text;
    bool matches;
} pattern_row;

TB_TEST(framework_pattern, matches_known_patterns)
{
    static const pattern_row rows[] = {
        /* Literals match only themselves, in full. */
        {"", "", true},
        {"", "a", false},
        {"a", "", false},
        {"sum.adds", "sum.adds", true},
        {"sum.adds", "sum.add", false},
        {"sum.add", "sum.adds", false},
        {"Sum.adds", "sum.adds", false},
        /* '?' matches exactly one character. */
        {"?", "", false},
        {"?", "a", true},
        {"?", "ab", false},
        {"a?c", "abc", true},
        {"a?c", "ac", false},
        {"order.?", "order.a", true},
        {"order.?", "order.ab", false},
        /* '*' matches any run, including the empty one. */
        {"*", "", true},
        {"*", "anything.at_all", true},
        {"**", "x", true},
        {"sum.*", "sum.adds", true},
        {"sum.*", "sum.", true},
        {"sum.*", "summary.adds", false},
        {"*.adds", "sum.adds", true},
        {"*.adds", "sum.adds_more", false},
        {"*sum*", "checksum.verify", true},
        {"*.*", "sum.adds", true},
        {"*.*", "nodot", false},
        {"a**b", "ab", true},
        {"a*b*c", "abc", true},
        {"a*b*c", "aXbYc", true},
        {"a*b*c", "aXbY", false},
        {"a*a", "a", false},
        /* A '*' must give characters back when the rest needs them. */
        {"*a", "aaa", true},
        {"*a", "aab", false},
        {"*ab", "aab", true},
        {"*aab", "aaab", true},
        {"a*ab", "aaab", true},
        {"s*m*s", "sum.adds", true},
        {"s*m*x", "sum.adds", false},
        /* Wildcards combine. */
        {"?*", "", false},
        {"?*", "x", true},
        {"*?", "x", true},
        {"*?*", "", false},
        {"*?*?*", "x", false},
        {"*?*?*", "xy", true},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu: pattern \"%s\", text \"%s\"", index, rows[index].pattern,
                        rows[index].text);
        TB_TEST_EXPECT(tb_test_pattern_matches(rows[index].pattern, rows[index].text) ==
                       rows[index].matches);
    }
}

/* The obvious recursive definition: slow, but hard to get wrong. */
static bool reference_matches(const char *pattern, const char *text)
{
    if (*pattern == '\0')
    {
        return *text == '\0';
    }
    if (*pattern == '*')
    {
        if (reference_matches(pattern + 1, text))
        {
            return true;
        }
        if (*text == '\0')
        {
            return false;
        }
        return reference_matches(pattern, text + 1);
    }
    if (*text == '\0')
    {
        return false;
    }
    if (*pattern == '?' || *pattern == *text)
    {
        return reference_matches(pattern + 1, text + 1);
    }
    return false;
}

/* Fills `buffer` with a random string of up to `maximum_length` characters. */
static void fill_random_string(char *buffer, size_t maximum_length, const char *alphabet)
{
    const size_t alphabet_size = strlen(alphabet);
    const size_t length = (size_t)tb_test_random_below(maximum_length + 1);

    for (size_t index = 0; index < length; index++)
    {
        buffer[index] = alphabet[tb_test_random_below(alphabet_size)];
    }
    buffer[length] = '\0';
}

TB_TEST(framework_pattern, agrees_with_reference_matcher_on_random_inputs)
{
    enum
    {
        ITERATION_COUNT = 50000,
        MAXIMUM_LENGTH = 9
    };
    char pattern[MAXIMUM_LENGTH + 1] = {0};
    char text[MAXIMUM_LENGTH + 1] = {0};

    /* Two letters keep random patterns and texts likely to nearly match,
     * which is where the backtracking is exercised. */
    for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++)
    {
        fill_random_string(pattern, MAXIMUM_LENGTH, "ab*?");
        fill_random_string(text, MAXIMUM_LENGTH, "ab");
        TB_TEST_CONTEXT("iteration %zu: pattern \"%s\", text \"%s\"", iteration, pattern, text);
        TB_TEST_EXPECT(tb_test_pattern_matches(pattern, text) == reference_matches(pattern, text));
    }
}

TB_TEST(framework_pattern, case_names_match_themselves_and_their_generalizations)
{
    enum
    {
        ITERATION_COUNT = 2000,
        MAXIMUM_LENGTH = 40
    };
    char name[MAXIMUM_LENGTH + 2] = {0};
    char pattern[MAXIMUM_LENGTH + 2] = {0};

    for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++)
    {
        size_t length = {0};
        size_t replaced = {0};

        fill_random_string(name, MAXIMUM_LENGTH, "abcxyz019_.");
        length = strlen(name);
        TB_TEST_CONTEXT("iteration %zu: name \"%s\"", iteration, name);

        TB_TEST_EXPECT(tb_test_pattern_matches(name, name));
        TB_TEST_EXPECT(tb_test_pattern_matches("*", name));

        /* One more literal character can never match. */
        memcpy(pattern, name, length);
        pattern[length] = 'q';
        pattern[length + 1] = '\0';
        TB_TEST_EXPECT(!tb_test_pattern_matches(pattern, name));

        if (length == 0)
        {
            continue;
        }
        /* Replacing any one character with '?' still matches. */
        replaced = (size_t)tb_test_random_below(length);
        memcpy(pattern, name, length + 1);
        pattern[replaced] = '?';
        TB_TEST_EXPECT(tb_test_pattern_matches(pattern, name));

        /* Replacing any tail with '*' still matches. */
        pattern[replaced] = '*';
        pattern[replaced + 1] = '\0';
        TB_TEST_EXPECT(tb_test_pattern_matches(pattern, name));
    }
}
