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
    TB_TEST_EXPECT_UINT(length, ==, strlen(text));
    TB_TEST_EXPECT_UINT(measured, ==, length);
    TB_TEST_EXPECT_STRING(allocated, ==, text);
    tb_str_free(NULL, allocated);
    return text;
}

/* The same format through the C library, for conversions on which the C
 * standard leaves nothing to the implementation. */
static const char *reference_text(const char *format, ...)
{
    static char text[TEXT_CAPACITY] = {0};
    va_list arguments = {0};

    va_start(arguments, format);
    (void)vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
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

/* The kinds of integer argument a conversion can name. */
typedef enum argument_kind
{
    ARGUMENT_INT,
    ARGUMENT_CHAR,
    ARGUMENT_SHORT,
    ARGUMENT_LONG,
    ARGUMENT_LONG_LONG,
    ARGUMENT_MAX,
    ARGUMENT_SIZE,
    ARGUMENT_KIND_COUNT
} argument_kind;

static const char *const length_letters[ARGUMENT_KIND_COUNT] = {"", "hh", "h", "l", "ll", "j", "z"};

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

/* Appends text to a format string being built. */
static void append(char *format, const char *text)
{
    memcpy(format + strlen(format), text, strlen(text) + 1);
}

/* Builds a random integer conversion: flags, width, precision, and length
 * that the C standard defines for the conversion character. */
static void build_integer_format(char *format, char conversion, argument_kind kind)
{
    static const char *const widths[] = {"", "", "1", "5", "12", "25"};
    static const char *const precisions[] = {"", "", ".", ".0", ".1", ".7", ".22"};
    const bool is_signed = conversion == 'd' || conversion == 'i';
    const bool has_alternate_form = conversion == 'o' || conversion == 'x' || conversion == 'X';
    char letter[2] = {conversion, '\0'};

    format[0] = '\0';
    append(format, "[%");
    if (tb_test_random_below(3) == 0)
    {
        append(format, "-");
    }
    if (tb_test_random_below(3) == 0)
    {
        append(format, "0");
    }
    if (is_signed && tb_test_random_below(3) == 0)
    {
        append(format, "+");
    }
    if (is_signed && tb_test_random_below(3) == 0)
    {
        append(format, " ");
    }
    if (has_alternate_form && tb_test_random_below(2) == 0)
    {
        append(format, "#");
    }
    append(format, widths[tb_test_random_below(TB_TEST_COUNT_OF(widths))]);
    append(format, precisions[tb_test_random_below(TB_TEST_COUNT_OF(precisions))]);
    append(format, length_letters[kind]);
    append(format, letter);
    append(format, "]");
}

/* Formats one value with both implementations and compares. The argument is
 * passed with the type its length names, to both alike. */
static void check_integer_format(const char *format, argument_kind kind, bool is_signed,
                                 uint64_t value)
{
    char expected[TEXT_CAPACITY] = {0};
    char actual[TEXT_CAPACITY] = {0};

    switch (kind)
    {
    case ARGUMENT_INT:
    case ARGUMENT_CHAR:
    case ARGUMENT_SHORT:
        /* Narrow arguments are passed as int, and narrowed by the conversion. */
        memcpy(expected, reference_text(format, (int)(unsigned)value), sizeof(expected));
        (void)tb_fmt_buffer(actual, sizeof(actual), format, (int)(unsigned)value);
        break;
    case ARGUMENT_LONG:
        memcpy(expected, reference_text(format, (long)(unsigned long)value), sizeof(expected));
        (void)tb_fmt_buffer(actual, sizeof(actual), format, (long)(unsigned long)value);
        break;
    case ARGUMENT_LONG_LONG:
        memcpy(expected, reference_text(format, (long long)value), sizeof(expected));
        (void)tb_fmt_buffer(actual, sizeof(actual), format, (long long)value);
        break;
    case ARGUMENT_MAX:
        memcpy(expected, reference_text(format, (intmax_t)value), sizeof(expected));
        (void)tb_fmt_buffer(actual, sizeof(actual), format, (intmax_t)value);
        break;
    case ARGUMENT_SIZE:
        memcpy(expected, reference_text(format, (size_t)value), sizeof(expected));
        (void)tb_fmt_buffer(actual, sizeof(actual), format, (size_t)value);
        break;
    case ARGUMENT_KIND_COUNT:
        break;
    }
    if (strcmp(actual, expected) != 0)
    {
        TB_TEST_FAIL("format \"%s\" of %s value 0x%llx gave \"%s\"; the C library gives \"%s\"",
                     format, is_signed ? "signed" : "unsigned", (unsigned long long)value, actual,
                     expected);
    }
}

TB_TEST(core_fmt, integer_conversions_agree_with_the_c_library_on_random_formats)
{
    enum
    {
        ITERATION_COUNT = 60000
    };
    static const char conversions[] = "diuoxX";

    for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++)
    {
        const char conversion = conversions[tb_test_random_below(sizeof(conversions) - 1)];
        const argument_kind kind = (argument_kind)tb_test_random_below(ARGUMENT_KIND_COUNT);
        char format[64] = {0};

        build_integer_format(format, conversion, kind);
        check_integer_format(format, kind, conversion == 'd' || conversion == 'i', random_value());
    }
}

TB_TEST(core_fmt, string_and_character_conversions_agree_with_the_c_library)
{
    static const char *const strings[] = {"", "a", "badger", "tenacious badger", "caf\xC3\xA9"};
    static const char *const shapes[] = {"",    "1",   "5",    "20",    "-5",   "-20",
                                         ".0",  ".1",  ".3",   ".40",   "9.3",  "-9.3",
                                         "20.", "3.5", "-3.5", "25.25", "1.20", "-1.0"};

    for (size_t shape = 0; shape < TB_TEST_COUNT_OF(shapes); shape++)
    {
        char string_format[32] = {0};
        char character_format[32] = {0};
        const bool has_precision = strchr(shapes[shape], '.') != NULL;

        append(string_format, "[%");
        append(string_format, shapes[shape]);
        append(string_format, "s]");
        for (size_t index = 0; index < TB_TEST_COUNT_OF(strings); index++)
        {
            char expected[TEXT_CAPACITY] = {0};
            char actual[TEXT_CAPACITY] = {0};

            memcpy(expected, reference_text(string_format, strings[index]), sizeof(expected));
            (void)tb_fmt_buffer(actual, sizeof(actual), string_format, strings[index]);
            TB_TEST_CONTEXT("format \"%s\" of \"%s\"", string_format, strings[index]);
            TB_TEST_EXPECT_STRING(actual, ==, expected);
        }
        /* The C standard gives a character conversion no precision. */
        if (has_precision)
        {
            continue;
        }
        append(character_format, "[%");
        append(character_format, shapes[shape]);
        append(character_format, "c]");
        for (int character = 1; character < 128; character += 7)
        {
            char expected[TEXT_CAPACITY] = {0};
            char actual[TEXT_CAPACITY] = {0};

            memcpy(expected, reference_text(character_format, character), sizeof(expected));
            (void)tb_fmt_buffer(actual, sizeof(actual), character_format, character);
            TB_TEST_CONTEXT("format \"%s\" of character %d", character_format, character);
            TB_TEST_EXPECT_STRING(actual, ==, expected);
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
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, strlen("badger #007") + 1);
    tb_str_free(&allocator, text);

    text = tb_fmt_string(&allocator, "");
    TB_TEST_EXPECT_STRING(text, ==, "");
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 1);
    tb_str_free(&allocator, text);

    /* Long output, far beyond any internal buffer. */
    text = tb_fmt_string(&allocator, "[%5000d|%-5000s]", 1, "x");
    TB_TEST_EXPECT_UINT(strlen(text), ==, 10003);
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
    TB_TEST_EXPECT_UINT(length, ==, strlen("width=640, flags=0xff; ") + 200 + 1);
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

    /* A new string is measured and then written: two calls, one allocation. */
    vector_conversion_calls = 0;
    text = tb_fmt_string(&allocator, "%V", &position);
    TB_TEST_EXPECT_STRING(text, ==, "(3, -4)");
    TB_TEST_EXPECT_UINT(vector_conversion_calls, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
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
    "%f",      /* reserved for floating point */
    "%.2e",
    "%g",
    "%Lf",
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

        memcpy(format, invalid_formats[index], strlen(invalid_formats[index]));
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
