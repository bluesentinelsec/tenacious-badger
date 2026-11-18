/* Formatting must produce the same text on every platform, so its output is
 * pinned three ways: tables of exact expected text; agreement with the C
 * library's printf for the integer, character, and string conversions, whose
 * output the C standard fully specifies; and exact byte counts for every
 * destination. Output to the standard streams is checked by fmt_probe.c in a
 * child process. */
#include "badger/fmt.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
    TEXT_CAPACITY = 512
};

/* Formats through every destination and checks that they agree with each
 * other and with the length each reports. Returns the text, which is valid
 * until the next call. */
static const char *format_text(const char *format, ...)
{
    static char text[TEXT_CAPACITY] = {0};
    char *allocated = NULL;
    va_list arguments = {0};
    size_t length = {0};
    size_t measured = {0};

    va_start(arguments, format);
    length = tb_fmt_vbuffer(text, sizeof(text), format, arguments);
    measured = tb_fmt_vbuffer(NULL, 0, format, arguments);
    allocated = tb_fmt_vstring(NULL, format, arguments);
    va_end(arguments);
    TB_TEST_EXPECT_UINT(length, ==, tb_str_length(text));
    TB_TEST_EXPECT_UINT(measured, ==, length);
    TB_TEST_EXPECT_STRING(allocated, ==, text);
    tb_str_free(NULL, allocated);
    return text;
}

/* --- plain text and percent signs --------------------------------------------- */

TB_TEST(core_fmt, text_without_conversions_is_copied)
{
    TB_TEST_EXPECT_STRING(format_text(""), ==, "");
    TB_TEST_EXPECT_STRING(format_text("badger"), ==, "badger");
    TB_TEST_EXPECT_STRING(format_text("line one\nline two\t."), ==, "line one\nline two\t.");
    TB_TEST_EXPECT_STRING(format_text("100%%"), ==, "100%");
    TB_TEST_EXPECT_STRING(format_text("%%"), ==, "%");
    TB_TEST_EXPECT_STRING(format_text("%%%%"), ==, "%%");
    TB_TEST_EXPECT_STRING(format_text("a%%b%%c"), ==, "a%b%c");
    TB_TEST_EXPECT_STRING(format_text("%%d"), ==, "%d");
    TB_TEST_EXPECT_STRING(format_text("caf\xC3\xA9 %%"), ==, "caf\xC3\xA9 %");
}

/* --- integers ----------------------------------------------------------------- */

TB_TEST(core_fmt, signed_integers_follow_their_flags_width_and_precision)
{
    TB_TEST_EXPECT_STRING(format_text("%d", 0), ==, "0");
    TB_TEST_EXPECT_STRING(format_text("%d", 42), ==, "42");
    TB_TEST_EXPECT_STRING(format_text("%d", -42), ==, "-42");
    TB_TEST_EXPECT_STRING(format_text("%i", 42), ==, "42");
    TB_TEST_EXPECT_STRING(format_text("[%5d]", 42), ==, "[   42]");
    TB_TEST_EXPECT_STRING(format_text("[%-5d]", 42), ==, "[42   ]");
    TB_TEST_EXPECT_STRING(format_text("[%05d]", 42), ==, "[00042]");
    TB_TEST_EXPECT_STRING(format_text("[%05d]", -42), ==, "[-0042]");
    TB_TEST_EXPECT_STRING(format_text("[%+d]", 42), ==, "[+42]");
    TB_TEST_EXPECT_STRING(format_text("[%+d]", -42), ==, "[-42]");
    TB_TEST_EXPECT_STRING(format_text("[% d]", 42), ==, "[ 42]");
    TB_TEST_EXPECT_STRING(format_text("[% d]", -42), ==, "[-42]");
    /* A plus sign takes precedence over a space. */
    TB_TEST_EXPECT_STRING(format_text("[%+ d]", 42), ==, "[+42]");
    TB_TEST_EXPECT_STRING(format_text("[%+05d]", 42), ==, "[+0042]");
    TB_TEST_EXPECT_STRING(format_text("[%.5d]", 42), ==, "[00042]");
    TB_TEST_EXPECT_STRING(format_text("[%.5d]", -42), ==, "[-00042]");
    TB_TEST_EXPECT_STRING(format_text("[%8.5d]", 42), ==, "[   00042]");
    TB_TEST_EXPECT_STRING(format_text("[%-8.5d]", 42), ==, "[00042   ]");
    /* A precision turns the zero flag off, and so does left justification. */
    TB_TEST_EXPECT_STRING(format_text("[%08.5d]", 42), ==, "[   00042]");
    TB_TEST_EXPECT_STRING(format_text("[%-05d]", 42), ==, "[42   ]");
    /* Zero with a precision of zero has no digits at all. */
    TB_TEST_EXPECT_STRING(format_text("[%.0d]", 0), ==, "[]");
    TB_TEST_EXPECT_STRING(format_text("[%.d]", 0), ==, "[]");
    TB_TEST_EXPECT_STRING(format_text("[%+.0d]", 0), ==, "[+]");
    TB_TEST_EXPECT_STRING(format_text("[%3.0d]", 0), ==, "[   ]");
    TB_TEST_EXPECT_STRING(format_text("[%.0d]", 7), ==, "[7]");
    /* A width narrower than the number does not truncate it. */
    TB_TEST_EXPECT_STRING(format_text("[%2d]", 12345), ==, "[12345]");
}

TB_TEST(core_fmt, unsigned_integers_are_written_in_three_bases)
{
    TB_TEST_EXPECT_STRING(format_text("%u", 42u), ==, "42");
    TB_TEST_EXPECT_STRING(format_text("%u", 4294967295u), ==, "4294967295");
    TB_TEST_EXPECT_STRING(format_text("%x", 255u), ==, "ff");
    TB_TEST_EXPECT_STRING(format_text("%X", 255u), ==, "FF");
    TB_TEST_EXPECT_STRING(format_text("%x", 0xDEADBEEFu), ==, "deadbeef");
    TB_TEST_EXPECT_STRING(format_text("%X", 0xDEADBEEFu), ==, "DEADBEEF");
    TB_TEST_EXPECT_STRING(format_text("%o", 8u), ==, "10");
    TB_TEST_EXPECT_STRING(format_text("%o", 0u), ==, "0");
    TB_TEST_EXPECT_STRING(format_text("[%8x]", 255u), ==, "[      ff]");
    TB_TEST_EXPECT_STRING(format_text("[%08x]", 255u), ==, "[000000ff]");
    TB_TEST_EXPECT_STRING(format_text("[%-8x]", 255u), ==, "[ff      ]");
    TB_TEST_EXPECT_STRING(format_text("[%.4x]", 255u), ==, "[00ff]");
    /* The alternate form marks the base, but not on zero in hexadecimal. */
    TB_TEST_EXPECT_STRING(format_text("%#x", 255u), ==, "0xff");
    TB_TEST_EXPECT_STRING(format_text("%#X", 255u), ==, "0XFF");
    TB_TEST_EXPECT_STRING(format_text("%#x", 0u), ==, "0");
    TB_TEST_EXPECT_STRING(format_text("[%#8x]", 255u), ==, "[    0xff]");
    TB_TEST_EXPECT_STRING(format_text("[%#08x]", 255u), ==, "[0x0000ff]");
    TB_TEST_EXPECT_STRING(format_text("[%#-8x]", 255u), ==, "[0xff    ]");
    TB_TEST_EXPECT_STRING(format_text("%#.4x", 255u), ==, "0x00ff");
    /* Octal gets one leading zero, unless the precision already gave one. */
    TB_TEST_EXPECT_STRING(format_text("%#o", 8u), ==, "010");
    TB_TEST_EXPECT_STRING(format_text("%#o", 0u), ==, "0");
    TB_TEST_EXPECT_STRING(format_text("%#.0o", 0u), ==, "0");
    TB_TEST_EXPECT_STRING(format_text("%#.5o", 8u), ==, "00010");
    TB_TEST_EXPECT_STRING(format_text("[%.0x]", 0u), ==, "[]");
    /* Sign flags mean nothing for an unsigned conversion. */
    TB_TEST_EXPECT_STRING(format_text("%+u", 42u), ==, "42");
    TB_TEST_EXPECT_STRING(format_text("% x", 255u), ==, "ff");
}

TB_TEST(core_fmt, every_integer_length_reaches_its_limits)
{
    TB_TEST_EXPECT_STRING(format_text("%hhd", -128), ==, "-128");
    TB_TEST_EXPECT_STRING(format_text("%hhd", 127), ==, "127");
    TB_TEST_EXPECT_STRING(format_text("%hhu", 255), ==, "255");
    TB_TEST_EXPECT_STRING(format_text("%hd", -32768), ==, "-32768");
    TB_TEST_EXPECT_STRING(format_text("%hu", 65535), ==, "65535");
    /* A shorter length keeps only the low bits of what was passed. */
    TB_TEST_EXPECT_STRING(format_text("%hhd", 0x1FF), ==, "-1");
    TB_TEST_EXPECT_STRING(format_text("%hhu", 0x1FF), ==, "255");
    TB_TEST_EXPECT_STRING(format_text("%hx", 0x12345), ==, "2345");
    TB_TEST_EXPECT_STRING(format_text("%d", INT_MIN), ==, "-2147483648");
    TB_TEST_EXPECT_STRING(format_text("%d", INT_MAX), ==, "2147483647");
    TB_TEST_EXPECT_STRING(format_text("%ld", -2147483647L), ==, "-2147483647");
    TB_TEST_EXPECT_STRING(format_text("%lu", 4294967295UL), ==, "4294967295");
    TB_TEST_EXPECT_STRING(format_text("%lld", LLONG_MIN), ==, "-9223372036854775808");
    TB_TEST_EXPECT_STRING(format_text("%lld", LLONG_MAX), ==, "9223372036854775807");
    TB_TEST_EXPECT_STRING(format_text("%llu", ULLONG_MAX), ==, "18446744073709551615");
    TB_TEST_EXPECT_STRING(format_text("%llx", ULLONG_MAX), ==, "ffffffffffffffff");
    TB_TEST_EXPECT_STRING(format_text("%llo", ULLONG_MAX), ==, "1777777777777777777777");
    TB_TEST_EXPECT_STRING(format_text("%#llo", ULLONG_MAX), ==, "01777777777777777777777");
    TB_TEST_EXPECT_STRING(format_text("%jd", INTMAX_MIN), ==, "-9223372036854775808");
    TB_TEST_EXPECT_STRING(format_text("%ju", UINTMAX_MAX), ==, "18446744073709551615");
    TB_TEST_EXPECT_STRING(format_text("%zu", (size_t)12345), ==, "12345");
    TB_TEST_EXPECT_STRING(format_text("%zx", (size_t)0xABCDEF), ==, "abcdef");
    TB_TEST_EXPECT_STRING(format_text("%zd", (ptrdiff_t)-12345), ==, "-12345");
    TB_TEST_EXPECT_STRING(format_text("%td", (ptrdiff_t)-7), ==, "-7");
    TB_TEST_EXPECT_STRING(format_text("%tu", (size_t)7), ==, "7");
    TB_TEST_EXPECT_STRING(format_text("%d %ld %lld %zu", 1, 2L, 3LL, (size_t)4), ==, "1 2 3 4");
}

TB_TEST(core_fmt, width_and_precision_can_come_from_arguments)
{
    TB_TEST_EXPECT_STRING(format_text("[%*d]", 6, 42), ==, "[    42]");
    TB_TEST_EXPECT_STRING(format_text("[%-*d]", 6, 42), ==, "[42    ]");
    /* A negative width asks for left justification. */
    TB_TEST_EXPECT_STRING(format_text("[%*d]", -6, 42), ==, "[42    ]");
    TB_TEST_EXPECT_STRING(format_text("[%.*d]", 4, 42), ==, "[0042]");
    /* A negative precision is no precision. */
    TB_TEST_EXPECT_STRING(format_text("[%.*d]", -4, 0), ==, "[0]");
    TB_TEST_EXPECT_STRING(format_text("[%0*.*d]", 6, -1, 42), ==, "[000042]");
    TB_TEST_EXPECT_STRING(format_text("[%*.*d]", 8, 4, 42), ==, "[    0042]");
    TB_TEST_EXPECT_STRING(format_text("[%.*s]", 3, "badger"), ==, "[bad]");
    TB_TEST_EXPECT_STRING(format_text("[%*s]", 8, "badger"), ==, "[  badger]");
    TB_TEST_EXPECT_STRING(format_text("[%*c]", 3, 'x'), ==, "[  x]");
    TB_TEST_EXPECT_STRING(format_text("[%*d|%*d]", 3, 1, -3, 2), ==, "[  1|2  ]");
}

/* --- characters, strings, pointers -------------------------------------------- */

TB_TEST(core_fmt, characters_and_strings_are_padded_and_cut)
{
    static const char unterminated[3] = {'a', 'b', 'c'};
    const char *const missing = NULL;

    TB_TEST_EXPECT_STRING(format_text("%c", 'x'), ==, "x");
    TB_TEST_EXPECT_STRING(format_text("[%3c]", 'x'), ==, "[  x]");
    TB_TEST_EXPECT_STRING(format_text("[%-3c]", 'x'), ==, "[x  ]");
    TB_TEST_EXPECT_STRING(format_text("%c%c%c", 'a', 'b', 'c'), ==, "abc");
    /* The argument is an int, of which the low byte is written. */
    TB_TEST_EXPECT_STRING(format_text("%c", 0x100 + 'q'), ==, "q");
    TB_TEST_EXPECT_STRING(format_text("%c", 0xE9), ==, "\xE9");

    TB_TEST_EXPECT_STRING(format_text("%s", ""), ==, "");
    TB_TEST_EXPECT_STRING(format_text("%s", "badger"), ==, "badger");
    TB_TEST_EXPECT_STRING(format_text("[%8s]", "badger"), ==, "[  badger]");
    TB_TEST_EXPECT_STRING(format_text("[%-8s]", "badger"), ==, "[badger  ]");
    TB_TEST_EXPECT_STRING(format_text("[%3s]", "badger"), ==, "[badger]");
    TB_TEST_EXPECT_STRING(format_text("[%.3s]", "badger"), ==, "[bad]");
    TB_TEST_EXPECT_STRING(format_text("[%.0s]", "badger"), ==, "[]");
    TB_TEST_EXPECT_STRING(format_text("[%.10s]", "badger"), ==, "[badger]");
    TB_TEST_EXPECT_STRING(format_text("[%6.2s]", "badger"), ==, "[    ba]");
    TB_TEST_EXPECT_STRING(format_text("[%-6.2s]", "badger"), ==, "[ba    ]");
    TB_TEST_EXPECT_STRING(format_text("%s and %s", "one", "two"), ==, "one and two");
    TB_TEST_EXPECT_STRING(format_text("%s", "caf\xC3\xA9"), ==, "caf\xC3\xA9");
    /* With a precision the string need not be terminated within it. */
    TB_TEST_EXPECT_STRING(format_text("[%.3s]", unterminated), ==, "[abc]");
    TB_TEST_EXPECT_STRING(format_text("[%.2s]", unterminated), ==, "[ab]");
    /* A missing string is named, not dereferenced. */
    TB_TEST_EXPECT_STRING(format_text("%s", missing), ==, "(null)");
    TB_TEST_EXPECT_STRING(format_text("[%.3s]", missing), ==, "[(nu]");
    TB_TEST_EXPECT_STRING(format_text("[%8s]", missing), ==, "[  (null)]");
}

TB_TEST(core_fmt, pointers_are_written_the_same_way_on_every_platform)
{
    void *const missing = NULL;
    void *const address = (void *)(uintptr_t)0x1A2B3C;

    TB_TEST_EXPECT_STRING(format_text("%p", missing), ==, "0x0");
    TB_TEST_EXPECT_STRING(format_text("%p", address), ==, "0x1a2b3c");
    TB_TEST_EXPECT_STRING(format_text("[%12p]", address), ==, "[    0x1a2b3c]");
    TB_TEST_EXPECT_STRING(format_text("[%-12p]", address), ==, "[0x1a2b3c    ]");
    TB_TEST_EXPECT_STRING(format_text("[%012p]", address), ==, "[0x00001a2b3c]");
}

/* --- agreement with the C library --------------------------------------------- */

/* The C standard leaves nothing to the implementation for the integer,
 * character, and string conversions, so the C library is the reference for
 * them. A compiler can check a format string only when it is a literal, and
 * rightly warns about one that is not. Every format compared here is
 * therefore a literal: its flags, length, and conversion are written out, and
 * its width, precision, and value vary at run time. */

static char expected_text[TEXT_CAPACITY] = {0};
static char actual_text[TEXT_CAPACITY] = {0};

/* Checks one format that both implementations have just written out. */
static void compare_with_c_library(const char *format, int expected_length, size_t actual_length)
{
    TB_TEST_CONTEXT("format \"%s\"", format);
    TB_TEST_EXPECT_STRING(actual_text, ==, expected_text);
    TB_TEST_EXPECT_INT(expected_length, ==, (int)actual_length);
}

/* Formats the same arguments with the C library and with the engine, and
 * compares the text and the length. The format must be a string literal, and
 * the arguments are evaluated twice, so they must be plain values. The
 * macros built on it take the `width` and `precision` of the loop around
 * them. */
#define EXPECT_SAME_AS_C_LIBRARY(format, ...)                                                      \
    compare_with_c_library(format,                                                                 \
                           snprintf(expected_text, sizeof(expected_text), format, __VA_ARGS__),    \
                           tb_fmt_buffer(actual_text, sizeof(actual_text), format, __VA_ARGS__))

/* One signed conversion with every set of flags the C standard defines for
 * it, with a width and then with a width and a precision. The zero flag is
 * left out where there is a precision, which turns it off; compilers warn
 * about that combination, as they do about a space with a plus sign. */
#define EXPECT_SIGNED_SAME(length, conversion, value)                                              \
    do                                                                                             \
    {                                                                                              \
        EXPECT_SAME_AS_C_LIBRARY("[%*" length conversion "]", width, (value));                     \
        EXPECT_SAME_AS_C_LIBRARY("[%-*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%0*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%+*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[% *" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%+0*" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[% 0*" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[%-+*" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[%- *" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[%*.*" length conversion "]", width, precision, (value));        \
        EXPECT_SAME_AS_C_LIBRARY("[%-*.*" length conversion "]", width, precision, (value));       \
        EXPECT_SAME_AS_C_LIBRARY("[%+*.*" length conversion "]", width, precision, (value));       \
        EXPECT_SAME_AS_C_LIBRARY("[% *.*" length conversion "]", width, precision, (value));       \
        EXPECT_SAME_AS_C_LIBRARY("[%-+*.*" length conversion "]", width, precision, (value));      \
    } while (0)

/* One unsigned conversion, likewise. The alternate form is defined for octal
 * and hexadecimal and not for decimal. */
#define EXPECT_UNSIGNED_SAME(length, conversion, value)                                            \
    do                                                                                             \
    {                                                                                              \
        EXPECT_SAME_AS_C_LIBRARY("[%*" length conversion "]", width, (value));                     \
        EXPECT_SAME_AS_C_LIBRARY("[%-*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%0*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%*.*" length conversion "]", width, precision, (value));        \
        EXPECT_SAME_AS_C_LIBRARY("[%-*.*" length conversion "]", width, precision, (value));       \
    } while (0)

#define EXPECT_ALTERNATE_SAME(length, conversion, value)                                           \
    do                                                                                             \
    {                                                                                              \
        EXPECT_UNSIGNED_SAME(length, conversion, value);                                           \
        EXPECT_SAME_AS_C_LIBRARY("[%#*" length conversion "]", width, (value));                    \
        EXPECT_SAME_AS_C_LIBRARY("[%#-*" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[%#0*" length conversion "]", width, (value));                   \
        EXPECT_SAME_AS_C_LIBRARY("[%#*.*" length conversion "]", width, precision, (value));       \
        EXPECT_SAME_AS_C_LIBRARY("[%#-*.*" length conversion "]", width, precision, (value));      \
    } while (0)

/* A value whose interesting cases are near zero, near the limits of each
 * width, and anywhere else. */
static uint64_t random_value(void)
{
    static const uint64_t limits[] = {
        0,
        0x7F,
        0x80,
        0xFF,
        0x7FFF,
        0x8000,
        0xFFFF,
        0x7FFFFFFF,
        0x80000000u,
        0xFFFFFFFFu,
        UINT64_C(0x7FFFFFFFFFFFFFFF),
        UINT64_C(0x8000000000000000),
        UINT64_MAX,
    };

    switch (tb_test_random_below(4))
    {
    case 0:
        return limits[tb_test_random_below(TB_TEST_COUNT_OF(limits))] + tb_test_random_below(3) - 1;
    case 1:
        return tb_test_random_below(1000);
    default:
        return tb_test_random_u64() >> tb_test_random_below(64);
    }
}

enum
{
    AGREEMENT_ITERATION_COUNT = 1500,
    /* Widths and precisions from below zero, which have meanings of their
     * own, to more than the digits of any integer. */
    SMALLEST_WIDTH = -12,
    LARGEST_WIDTH = 26,
    SMALLEST_PRECISION = -3,
    LARGEST_PRECISION = 24
};

TB_TEST(core_fmt, signed_conversions_agree_with_the_c_library_on_random_values)
{
    for (size_t iteration = 0; iteration < AGREEMENT_ITERATION_COUNT; iteration++)
    {
        const int width = (int)tb_test_random_between(SMALLEST_WIDTH, LARGEST_WIDTH);
        const int precision = (int)tb_test_random_between(SMALLEST_PRECISION, LARGEST_PRECISION);
        const uint64_t value = random_value();

        EXPECT_SIGNED_SAME("", "d", (int)value);
        EXPECT_SIGNED_SAME("", "i", (int)value);
        EXPECT_SIGNED_SAME("hh", "d", (signed char)value);
        EXPECT_SIGNED_SAME("h", "d", (short)value);
        EXPECT_SIGNED_SAME("l", "d", (long)value);
        EXPECT_SIGNED_SAME("ll", "d", (long long)value);
        EXPECT_SIGNED_SAME("j", "d", (intmax_t)value);
        EXPECT_SIGNED_SAME("t", "d", (ptrdiff_t)value);
    }
}

TB_TEST(core_fmt, unsigned_conversions_agree_with_the_c_library_on_random_values)
{
    for (size_t iteration = 0; iteration < AGREEMENT_ITERATION_COUNT; iteration++)
    {
        const int width = (int)tb_test_random_between(SMALLEST_WIDTH, LARGEST_WIDTH);
        const int precision = (int)tb_test_random_between(SMALLEST_PRECISION, LARGEST_PRECISION);
        const uint64_t value = random_value();

        EXPECT_UNSIGNED_SAME("", "u", (unsigned)value);
        EXPECT_UNSIGNED_SAME("hh", "u", (unsigned char)value);
        EXPECT_UNSIGNED_SAME("h", "u", (unsigned short)value);
        EXPECT_UNSIGNED_SAME("l", "u", (unsigned long)value);
        EXPECT_UNSIGNED_SAME("ll", "u", (unsigned long long)value);
        EXPECT_UNSIGNED_SAME("j", "u", (uintmax_t)value);
        EXPECT_UNSIGNED_SAME("z", "u", (size_t)value);
    }
}

TB_TEST(core_fmt, octal_and_hexadecimal_conversions_agree_with_the_c_library_on_random_values)
{
    for (size_t iteration = 0; iteration < AGREEMENT_ITERATION_COUNT; iteration++)
    {
        const int width = (int)tb_test_random_between(SMALLEST_WIDTH, LARGEST_WIDTH);
        const int precision = (int)tb_test_random_between(SMALLEST_PRECISION, LARGEST_PRECISION);
        const uint64_t value = random_value();

        EXPECT_ALTERNATE_SAME("", "o", (unsigned)value);
        EXPECT_ALTERNATE_SAME("", "x", (unsigned)value);
        EXPECT_ALTERNATE_SAME("", "X", (unsigned)value);
        EXPECT_ALTERNATE_SAME("hh", "x", (unsigned char)value);
        EXPECT_ALTERNATE_SAME("h", "o", (unsigned short)value);
        EXPECT_ALTERNATE_SAME("l", "X", (unsigned long)value);
        EXPECT_ALTERNATE_SAME("ll", "o", (unsigned long long)value);
        EXPECT_ALTERNATE_SAME("ll", "x", (unsigned long long)value);
        EXPECT_ALTERNATE_SAME("j", "X", (uintmax_t)value);
        EXPECT_ALTERNATE_SAME("z", "x", (size_t)value);
    }
}

TB_TEST(core_fmt, string_and_character_conversions_agree_with_the_c_library)
{
    static const char *const strings[] = {"", "a", "badger", "tenacious badger", "caf\xC3\xA9"};

    for (int width = SMALLEST_WIDTH; width <= LARGEST_WIDTH; width++)
    {
        for (int precision = SMALLEST_PRECISION; precision <= LARGEST_PRECISION; precision++)
        {
            for (size_t index = 0; index < TB_TEST_COUNT_OF(strings); index++)
            {
                EXPECT_SAME_AS_C_LIBRARY("[%*s]", width, strings[index]);
                EXPECT_SAME_AS_C_LIBRARY("[%-*s]", width, strings[index]);
                EXPECT_SAME_AS_C_LIBRARY("[%*.*s]", width, precision, strings[index]);
                EXPECT_SAME_AS_C_LIBRARY("[%-*.*s]", width, precision, strings[index]);
            }
        }
        for (int character = 1; character < 128; character += 7)
        {
            EXPECT_SAME_AS_C_LIBRARY("[%*c]", width, character);
            EXPECT_SAME_AS_C_LIBRARY("[%-*c]", width, character);
        }
    }
}

/* --- destinations ------------------------------------------------------------- */

TB_TEST(core_fmt, a_buffer_is_filled_as_far_as_it_goes_and_always_terminated)
{
    static const char full[] = "badger 42!";
    const size_t full_length = sizeof(full) - 1;

    for (size_t size = 0; size <= full_length + 5; size++)
    {
        char buffer[32] = {0};
        /* Bytes of the text that fit before the terminator. */
        size_t kept = full_length;
        size_t length = {0};

        if (size <= full_length)
        {
            kept = size == 0 ? 0 : size - 1;
        }
        memset(buffer, '#', sizeof(buffer));
        length = tb_fmt_buffer(buffer, size, "%s %d!", "badger", 42);
        TB_TEST_CONTEXT("buffer of %zu bytes", size);
        /* The whole length is reported, which tells whether it fit. */
        TB_TEST_EXPECT_UINT(length, ==, full_length);
        TB_TEST_EXPECT(length < size || kept < full_length);
        TB_TEST_EXPECT_BYTES(buffer, full, kept);
        if (size > 0)
        {
            TB_TEST_EXPECT_INT(buffer[kept], ==, '\0');
        }
        /* Nothing is written past the buffer. */
        for (size_t index = size; index < sizeof(buffer); index++)
        {
            TB_TEST_EXPECT_INT(buffer[index], ==, '#');
        }
    }
    TB_TEST_EXPECT_UINT(tb_fmt_buffer(NULL, 0, "%s %d!", "badger", 42), ==, full_length);
}

TB_TEST(core_fmt, a_new_string_is_exactly_as_long_as_its_text)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator in_arena = tb_arena_allocator(arena);
    char *text = tb_fmt_string(&allocator, "%s #%03d", "badger", 7);

    TB_TEST_EXPECT_STRING(text, ==, "badger #007");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, tb_str_length("badger #007") + 1);
    tb_str_free(&allocator, text);

    text = tb_fmt_string(&allocator, "");
    TB_TEST_EXPECT_STRING(text, ==, "");
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 1);
    tb_str_free(&allocator, text);

    /* Long output, far beyond any internal buffer. */
    text = tb_fmt_string(&allocator, "[%5000d|%-5000s]", 1, "x");
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, 10003);
    TB_TEST_EXPECT_INT(text[5000], ==, '1');
    TB_TEST_EXPECT_INT(text[5002], ==, 'x');
    TB_TEST_EXPECT_INT(text[5003], ==, ' ');
    TB_TEST_EXPECT_INT(text[10002], ==, ']');
    tb_str_free(&allocator, text);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);

    TB_TEST_EXPECT_STRING(tb_fmt_string(&in_arena, "%d-%d", 4, 2), ==, "4-2");
    tb_arena_destroy(arena);
}

/* Everything a sink was given. */
typedef struct sink_record
{
    char text[TEXT_CAPACITY];
    size_t length;
    size_t call_count;
    size_t empty_call_count;
} sink_record;

static void record_output(void *context, const char *bytes, size_t length)
{
    sink_record *const record = context;

    record->call_count++;
    if (length == 0)
    {
        record->empty_call_count++;
    }
    if (length <= sizeof(record->text) - 1 - record->length)
    {
        memcpy(record->text + record->length, bytes, length);
        record->length += length;
    }
}

TB_TEST(core_fmt, a_sink_receives_the_output_in_order)
{
    sink_record record = {0};
    size_t length = {0};

    length = tb_fmt_write(record_output, &record, "%s=%d, %s=%#x; %200s|", "width", 640, "flags",
                          255u, "end");
    TB_TEST_EXPECT_UINT(length, ==, record.length);
    TB_TEST_EXPECT_UINT(length, ==, tb_str_length("width=640, flags=0xff; ") + 200 + 1);
    TB_TEST_EXPECT(memcmp(record.text, "width=640, flags=0xff; ", 23) == 0);
    TB_TEST_EXPECT_STRING(record.text + record.length - 4, ==, "end|");
    TB_TEST_EXPECT_UINT(record.call_count, >, 1);
    /* A sink is never bothered with nothing. */
    TB_TEST_EXPECT_UINT(record.empty_call_count, ==, 0);

    memset(&record, 0, sizeof(record));
    TB_TEST_EXPECT_UINT(tb_fmt_write(record_output, &record, ""), ==, 0);
    TB_TEST_EXPECT_UINT(tb_fmt_write(record_output, &record, "%s%.0d", "", 0), ==, 0);
    TB_TEST_EXPECT_UINT(record.call_count, ==, 0);
}

static size_t write_with_list(sink_record *record, const char *format, ...)
{
    va_list arguments = {0};
    size_t first = {0};
    size_t second = {0};

    va_start(arguments, format);
    /* The list is left as it was, so it can be used again. */
    first = tb_fmt_vwrite(record_output, record, format, arguments);
    second = tb_fmt_vwrite(record_output, record, format, arguments);
    va_end(arguments);
    return first + second;
}

TB_TEST(core_fmt, the_list_forms_leave_their_argument_list_usable)
{
    sink_record record = {0};

    TB_TEST_EXPECT_UINT(write_with_list(&record, "%d-%s;", 7, "x"), ==, 8);
    TB_TEST_EXPECT_STRING(record.text, ==, "7-x;7-x;");
}

/* --- conversions added by the caller ------------------------------------------ */

typedef struct vector
{
    int x;
    int y;
} vector;

static size_t vector_conversion_calls = {0};

/* "%V" takes a pointer to a vector. */
static void format_vector(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const vector *const value = va_arg(*arguments, const vector *);
    char text[64] = {0};
    const size_t length = tb_fmt_buffer(text, sizeof(text), "(%d, %d)", value->x, value->y);

    vector_conversion_calls++;
    tb_fmt_put_padded(output, spec, text, length);
}

/* "%R" takes two ints and writes them as a ratio, reporting its spec. */
static void format_ratio(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const int numerator = va_arg(*arguments, int);
    const int denominator = va_arg(*arguments, int);
    char text[64] = {0};
    size_t length = {0};

    length = tb_fmt_buffer(text, sizeof(text), "%d/%d", numerator, denominator);
    tb_fmt_put(output, text, length);
    if (spec->alternate)
    {
        tb_fmt_put(output, "!", 1);
    }
    if (spec->has_precision)
    {
        length = tb_fmt_buffer(text, sizeof(text), "~%zu", spec->precision);
        tb_fmt_put(output, text, length);
    }
    tb_fmt_put(output, NULL, 0);
}

TB_TEST(core_fmt, a_registered_conversion_formats_a_type_of_the_caller)
{
    const vector position = {.x = 3, .y = -4};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    char *text = NULL;

    TB_TEST_EXPECT(tb_fmt_register('V', format_vector) == NULL);
    TB_TEST_EXPECT(tb_fmt_register('R', format_ratio) == NULL);
    TB_TEST_EXPECT_STRING(format_text("at %V.", &position), ==, "at (3, -4).");
    TB_TEST_EXPECT_STRING(format_text("[%10V]", &position), ==, "[   (3, -4)]");
    TB_TEST_EXPECT_STRING(format_text("[%-10V]", &position), ==, "[(3, -4)   ]");
    /* A conversion takes as many arguments as it needs, and what follows it
     * still gets the right ones. */
    TB_TEST_EXPECT_STRING(format_text("%R then %d", 3, 4, 5), ==, "3/4 then 5");
    TB_TEST_EXPECT_STRING(format_text("%#.2R", 1, 2), ==, "1/2!~2");
    TB_TEST_EXPECT_STRING(format_text("%d %V %s", 1, &position, "end"), ==, "1 (3, -4) end");

    /* A short new string is formatted once and copied: one call, one
     * allocation. */
    vector_conversion_calls = 0;
    text = tb_fmt_string(&allocator, "%V", &position);
    TB_TEST_EXPECT_STRING(text, ==, "(3, -4)");
    TB_TEST_EXPECT_UINT(vector_conversion_calls, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
    tb_str_free(&allocator, text);

    /* So is the longest text that fits the buffer it is first formatted
     * into, 255 bytes and a terminator. */
    vector_conversion_calls = 0;
    text = tb_fmt_string(&allocator, "%255V", &position);
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, 255);
    TB_TEST_EXPECT_STRING(text + 248, ==, "(3, -4)");
    TB_TEST_EXPECT_INT(text[0], ==, ' ');
    TB_TEST_EXPECT_INT(text[247], ==, ' ');
    TB_TEST_EXPECT_UINT(vector_conversion_calls, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 256);
    tb_str_free(&allocator, text);

    /* One byte more does not fit: the text is measured and then written, two
     * calls, and still one allocation of exactly its size. */
    vector_conversion_calls = 0;
    text = tb_fmt_string(&allocator, "%256V", &position);
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, 256);
    TB_TEST_EXPECT_STRING(text + 249, ==, "(3, -4)");
    TB_TEST_EXPECT_INT(text[0], ==, ' ');
    TB_TEST_EXPECT_INT(text[248], ==, ' ');
    TB_TEST_EXPECT_UINT(vector_conversion_calls, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 257);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 3);
    tb_str_free(&allocator, text);

    /* Registering again returns what was there; NULL removes it. */
    TB_TEST_EXPECT(tb_fmt_register('V', format_ratio) == format_vector);
    TB_TEST_EXPECT_STRING(format_text("%V", 5, 6), ==, "5/6");
    TB_TEST_EXPECT(tb_fmt_register('V', NULL) == format_ratio);
    TB_TEST_EXPECT(tb_fmt_register('V', NULL) == NULL);
    TB_TEST_EXPECT(tb_fmt_register('R', NULL) == format_ratio);
}

/* --- invalid formats ---------------------------------------------------------- */

static const char *const invalid_formats[] = {
    "%",       /* ends inside a conversion */
    "50%",     /* likewise, after text */
    "%5",      /* ends after a width */
    "%-08.3l", /* ends after a length */
    "%q",      /* no such conversion */
    "%V",      /* a conversion nobody has registered */
    "%Lf",     /* a long double, which the formatter does not take */
    "%LA",
    "%hf", /* an integer length on a floating-point conversion */
    "%llg",
    "%ze",
    "%n",  /* never supported */
    "%ls", /* a length on a conversion that takes none */
    "%hc",
    "%lp",
    "%99999999999999999999999d", /* a width that fits in nothing */
    "%.99999999999999999999999d",
    "%5%", /* a percent sign takes no width */
    "%!",
};

static void format_invalid(void *context)
{
    char buffer[64] = {0};

    (void)tb_fmt_buffer(buffer, sizeof(buffer), context, 1, 2, 3);
}

TB_TEST(core_fmt, an_invalid_conversion_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(invalid_formats); index++)
    {
        char format[64] = {0};

        memcpy(format, invalid_formats[index], tb_str_length(invalid_formats[index]));
        TB_TEST_CONTEXT("format \"%s\"", format);
        TB_TEST_EXPECT_TRAP(format_invalid, format, "the format string has an invalid conversion");
    }
}

TB_TEST(core_fmt, a_byte_outside_ascii_is_never_a_conversion)
{
    /* The byte 128 above 'V'. A registry that wrapped around would find the
     * conversion registered for 'V'. */
    char format[] = "%\xD6";

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT(tb_fmt_register('V', format_vector) == NULL);
    TB_TEST_EXPECT_TRAP(format_invalid, format, "the format string has an invalid conversion");
    TB_TEST_EXPECT(tb_fmt_register('V', NULL) == format_vector);
}

TB_TEST(core_fmt, an_invalid_conversion_is_written_as_it_stands_without_assertions)
{
    if (tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built with assertions, which reject the format first");
    }
    /* Formats that take no arguments, so nothing is read that was not passed. */
    TB_TEST_EXPECT_STRING(format_text("50%"), ==, "50%");
    TB_TEST_EXPECT_STRING(format_text("%q"), ==, "%q");
    TB_TEST_EXPECT_STRING(format_text("a %q b"), ==, "a %q b");
    TB_TEST_EXPECT_STRING(format_text("%-5q|"), ==, "%-5q|");
    TB_TEST_EXPECT_STRING(format_text("%5"), ==, "%5");
    TB_TEST_EXPECT_STRING(format_text("%!"), ==, "%!");
    /* What comes after an invalid conversion is still formatted. */
    TB_TEST_EXPECT_STRING(format_text("%q %%"), ==, "%q %");
    /* A byte outside ASCII is not a conversion, whatever is registered. */
    (void)tb_fmt_register('V', format_vector);
    TB_TEST_EXPECT_STRING(format_text("%\xD6"), ==, "%\xD6");
    (void)tb_fmt_register('V', NULL);
}

/* --- contract violations ------------------------------------------------------ */

static void ignore_output(void *context, const char *bytes, size_t length)
{
    (void)context;
    (void)bytes;
    (void)length;
}

static void write_without_a_sink(void *context)
{
    (void)context;
    (void)tb_fmt_write(NULL, NULL, "text");
}

static void write_without_a_format(void *context)
{
    (void)context;
    (void)tb_fmt_write(ignore_output, NULL, NULL);
}

static void buffer_without_a_format(void *context)
{
    (void)tb_fmt_buffer(context, 8, NULL);
}

static void buffer_without_a_buffer(void *context)
{
    (void)context;
    (void)tb_fmt_buffer(NULL, 8, "text");
}

static void string_without_a_format(void *context)
{
    (void)context;
    (void)tb_fmt_string(NULL, NULL);
}

static void register_a_character(void *context)
{
    const char *const character = context;

    (void)tb_fmt_register(*character, format_vector);
}

static void put_without_an_output(void *context)
{
    (void)context;
    tb_fmt_put(NULL, "x", 1);
}

TB_TEST(core_fmt, missing_arguments_violate_the_contract)
{
    char buffer[8] = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(write_without_a_sink, NULL, "sink != NULL");
    TB_TEST_EXPECT_TRAP(write_without_a_format, NULL, "format != NULL");
    TB_TEST_EXPECT_TRAP(buffer_without_a_format, buffer, "format != NULL");
    TB_TEST_EXPECT_TRAP(buffer_without_a_buffer, NULL, "buffer != NULL || size == 0");
    TB_TEST_EXPECT_TRAP(string_without_a_format, NULL, "format != NULL");
    TB_TEST_EXPECT_TRAP(put_without_an_output, NULL, "output != NULL");
}

TB_TEST(core_fmt, only_free_letters_can_be_registered)
{
    static const char taken[] = "diuxXocspaAeEfFgGnhljztL%0 5.*-+#";
    static const char available[] = "bBkKmMqQrRvVwWyYT";

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < sizeof(taken) - 1; index++)
    {
        char character = taken[index];

        TB_TEST_CONTEXT("character '%c'", character);
        TB_TEST_EXPECT_TRAP(register_a_character, &character, "the character must be a letter");
    }
    for (size_t index = 0; index < sizeof(available) - 1; index++)
    {
        TB_TEST_CONTEXT("character '%c'", available[index]);
        TB_TEST_EXPECT(tb_fmt_register(available[index], format_vector) == NULL);
        TB_TEST_EXPECT(tb_fmt_register(available[index], NULL) == format_vector);
    }
}

/* --- running out of memory ---------------------------------------------------- */

static void format_a_string(void *context)
{
    (void)tb_fmt_string(context, "%s #%d", "badger", 7);
}

TB_TEST(core_fmt, a_string_that_cannot_be_allocated_ends_the_process)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);

    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(format_a_string, &allocator, "out of memory: 1 x 10 bytes");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}
