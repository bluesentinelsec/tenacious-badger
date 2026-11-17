/* One pass over the format string. Text between conversions is written as it
 * stands. Each conversion is parsed into a tb_fmt_spec, its argument is taken
 * from the argument list, and its text is written through tb_fmt_output,
 * which counts the bytes and passes them to the sink, if there is one. */
#include "badger/fmt.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "badger/str.h"
#include "memory_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct tb_fmt_output
{
    tb_fmt_sink *sink; /* Receives the output; NULL to only count it. */
    void *context;     /* Passed to the sink. */
    size_t length;     /* Bytes written so far. */
};

/* Conversions are registered for ASCII characters only. */
#define TB_FMT_REGISTRY_SIZE 128

/* An octal digit holds three bits, so this is room for every digit of the
 * widest integer in the smallest base. */
#define TB_FMT_DIGIT_CAPACITY (sizeof(uintmax_t) * CHAR_BIT / 3 + 1)

/* Padding is written from a block of this many equal bytes. */
#define TB_FMT_PADDING_BLOCK 32

/* Bits of a number that one digit holds in hexadecimal and in octal. */
#define TB_FMT_HEXADECIMAL_DIGIT_BITS 4u
#define TB_FMT_OCTAL_DIGIT_BITS 3u

/* A new string whose text fits in a buffer of this size, terminator included,
 * is formatted once, into that buffer on the stack. Longer text is measured
 * first. Most strings a program formats are far shorter. */
#define TB_FMT_LOCAL_CAPACITY 256

static tb_fmt_conversion *tb_fmt_registry[TB_FMT_REGISTRY_SIZE] = {0};

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

void tb_fmt_put(tb_fmt_output *output, const char *bytes, size_t length)
{
    TB_ASSERT(output != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    if (length == 0)
    {
        return;
    }
    output->length += length;
    if (output->sink != NULL)
    {
        output->sink(output->context, bytes, length);
    }
}

/* Writes `count` copies of one byte. */
static void tb_fmt_put_repeated(tb_fmt_output *output, char byte, size_t count)
{
    char block[TB_FMT_PADDING_BLOCK] = {0};

    /* Most conversions have no padding, and need no block of it. */
    if (count == 0)
    {
        return;
    }
    memset(block, byte, sizeof(block));
    while (count > sizeof(block))
    {
        tb_fmt_put(output, block, sizeof(block));
        count -= sizeof(block);
    }
    tb_fmt_put(output, block, count);
}

/* Spaces needed to bring `length` bytes up to the width of a spec. */
static size_t tb_fmt_padding(const tb_fmt_spec *spec, size_t length)
{
    if (spec->width <= length)
    {
        return 0;
    }
    return spec->width - length;
}

void tb_fmt_put_padded(tb_fmt_output *output, const tb_fmt_spec *spec, const char *bytes,
                       size_t length)
{
    size_t padding = {0};

    TB_ASSERT(output != NULL);
    TB_ASSERT(spec != NULL);
    padding = tb_fmt_padding(spec, length);
    if (!spec->left_justify)
    {
        tb_fmt_put_repeated(output, ' ', padding);
    }
    tb_fmt_put(output, bytes, length);
    if (spec->left_justify)
    {
        tb_fmt_put_repeated(output, ' ', padding);
    }
}

/* ---------------------------------------------------------------------------
 * Integers.
 * ------------------------------------------------------------------------- */

/* How an integer is to be written, beyond what its spec says. */
typedef struct tb_fmt_integer
{
    uintmax_t magnitude; /* The value without its sign. */
    char sign;           /* '-', '+', or ' ' to write before it; 0 for none. */
    unsigned base;       /* 8, 10, or 16. */
    bool uppercase;      /* Capital letters for digits above nine. */
    bool forces_prefix;  /* Write "0x" even for zero, as a pointer does. */
} tb_fmt_integer;

/* The functions below write the digits of a number so that the last one is
 * just before `end`, and return where the first one is. Zero has no digits:
 * the precision supplies its one. Each divides by a constant, which a
 * compiler turns into a multiplication or a shift; dividing by a base held in
 * a variable costs a division instruction for every digit. */

/* Decimal, two digits at a time: half as many divisions, and the two digits
 * of every number below a hundred are read from a table. */
static char *tb_fmt_decimal_digits(char *end, uintmax_t value)
{
    static const char pairs[] = "00010203040506070809"
                                "10111213141516171819"
                                "20212223242526272829"
                                "30313233343536373839"
                                "40414243444546474849"
                                "50515253545556575859"
                                "60616263646566676869"
                                "70717273747576777879"
                                "80818283848586878889"
                                "90919293949596979899";
    char *first = end;

    while (value >= 100)
    {
        const size_t pair = (size_t)(value % 100);

        value /= 100;
        first -= 2;
        first[0] = pairs[pair * 2];
        first[1] = pairs[pair * 2 + 1];
    }
    if (value >= 10)
    {
        first -= 2;
        first[0] = pairs[value * 2];
        first[1] = pairs[value * 2 + 1];
        return first;
    }
    if (value != 0)
    {
        first--;
        first[0] = (char)('0' + (int)value);
    }
    return first;
}

/* A base that is a power of two: each digit is the next few bits. */
static char *tb_fmt_binary_digits(char *end, uintmax_t value, unsigned bits_per_digit,
                                  const char *alphabet)
{
    const uintmax_t digit_mask = ((uintmax_t)1 << bits_per_digit) - 1;
    char *first = end;

    while (value != 0)
    {
        first--;
        first[0] = alphabet[value & digit_mask];
        value >>= bits_per_digit;
    }
    return first;
}

static void tb_fmt_put_integer(tb_fmt_output *output, const tb_fmt_spec *spec,
                               const tb_fmt_integer *integer)
{
    static const char lowercase_digits[] = "0123456789abcdef";
    static const char uppercase_digits[] = "0123456789ABCDEF";
    const char *const alphabet = integer->uppercase ? uppercase_digits : lowercase_digits;
    char digits[TB_FMT_DIGIT_CAPACITY] = {0};
    char prefix[3] = {0};
    char *const digits_end = digits + sizeof(digits);
    const char *first_digit = NULL;
    size_t digit_count = {0};
    size_t prefix_length = {0};
    size_t zero_count = {0};
    size_t minimum_digit_count = 1;
    size_t padding = {0};

    if (integer->base == 10)
    {
        first_digit = tb_fmt_decimal_digits(digits_end, integer->magnitude);
    }
    else if (integer->base == 16)
    {
        first_digit = tb_fmt_binary_digits(digits_end, integer->magnitude,
                                           TB_FMT_HEXADECIMAL_DIGIT_BITS, alphabet);
    }
    else
    {
        first_digit =
            tb_fmt_binary_digits(digits_end, integer->magnitude, TB_FMT_OCTAL_DIGIT_BITS, alphabet);
    }
    digit_count = (size_t)(digits_end - first_digit);

    /* The precision is the least number of digits, made up with zeros. */
    if (spec->has_precision)
    {
        minimum_digit_count = spec->precision;
    }
    if (minimum_digit_count > digit_count)
    {
        zero_count = minimum_digit_count - digit_count;
    }
    /* The alternate form of octal starts with a zero, adding one only when
     * the precision has not already put one there. */
    if (spec->alternate && integer->base == 8 && zero_count == 0)
    {
        zero_count = 1;
    }

    if (integer->sign != 0)
    {
        prefix[prefix_length] = integer->sign;
        prefix_length++;
    }
    if (integer->base == 16 &&
        (integer->forces_prefix || (spec->alternate && integer->magnitude != 0)))
    {
        prefix[prefix_length] = '0';
        prefix[prefix_length + 1] = integer->uppercase ? 'X' : 'x';
        prefix_length += 2;
    }

    /* What is left of the width is padding: zeros after the prefix when the
     * zero flag applies, and spaces on one side otherwise. A precision or
     * left justification turns the zero flag off, as in C. */
    padding = tb_fmt_padding(spec, prefix_length + zero_count + digit_count);
    if (spec->zero_pad && !spec->left_justify && !spec->has_precision)
    {
        zero_count += padding;
        padding = 0;
    }
    if (!spec->left_justify)
    {
        tb_fmt_put_repeated(output, ' ', padding);
    }
    tb_fmt_put(output, prefix, prefix_length);
    tb_fmt_put_repeated(output, '0', zero_count);
    tb_fmt_put(output, first_digit, digit_count);
    if (spec->left_justify)
    {
        tb_fmt_put_repeated(output, ' ', padding);
    }
}

/* Takes a signed integer argument of the length a spec names. */
static intmax_t tb_fmt_take_signed(const tb_fmt_spec *spec, va_list *arguments)
{
    switch (spec->length)
    {
    case TB_FMT_LENGTH_CHAR:
        return (signed char)va_arg(*arguments, int);
    case TB_FMT_LENGTH_SHORT:
        return (short)va_arg(*arguments, int);
    case TB_FMT_LENGTH_LONG:
        return va_arg(*arguments, long);
    case TB_FMT_LENGTH_LONG_LONG:
        return va_arg(*arguments, long long);
    case TB_FMT_LENGTH_MAX:
        return va_arg(*arguments, intmax_t);
    case TB_FMT_LENGTH_SIZE:
    case TB_FMT_LENGTH_POINTER:
        return va_arg(*arguments, ptrdiff_t);
    case TB_FMT_LENGTH_DEFAULT:
        break;
    }
    return va_arg(*arguments, int);
}

/* Takes an unsigned integer argument of the length a spec names. */
static uintmax_t tb_fmt_take_unsigned(const tb_fmt_spec *spec, va_list *arguments)
{
    switch (spec->length)
    {
    case TB_FMT_LENGTH_CHAR:
        return (unsigned char)va_arg(*arguments, unsigned int);
    case TB_FMT_LENGTH_SHORT:
        return (unsigned short)va_arg(*arguments, unsigned int);
    case TB_FMT_LENGTH_LONG:
        return va_arg(*arguments, unsigned long);
    case TB_FMT_LENGTH_LONG_LONG:
        return va_arg(*arguments, unsigned long long);
    case TB_FMT_LENGTH_MAX:
        return va_arg(*arguments, uintmax_t);
    case TB_FMT_LENGTH_SIZE:
    case TB_FMT_LENGTH_POINTER:
        return va_arg(*arguments, size_t);
    case TB_FMT_LENGTH_DEFAULT:
        break;
    }
    return va_arg(*arguments, unsigned int);
}

static void tb_fmt_convert_signed(tb_fmt_output *output, const tb_fmt_spec *spec,
                                  va_list *arguments)
{
    const intmax_t value = tb_fmt_take_signed(spec, arguments);
    tb_fmt_integer integer = {.magnitude = (uintmax_t)value, .base = 10};

    if (value < 0)
    {
        /* Negated as an unsigned value, which is defined for the most
         * negative integer as well. */
        integer.magnitude = (uintmax_t)0 - (uintmax_t)value;
        integer.sign = '-';
    }
    else if (spec->show_sign)
    {
        integer.sign = '+';
    }
    else if (spec->space_sign)
    {
        integer.sign = ' ';
    }
    tb_fmt_put_integer(output, spec, &integer);
}

static void tb_fmt_convert_unsigned(tb_fmt_output *output, const tb_fmt_spec *spec,
                                    va_list *arguments)
{
    tb_fmt_integer integer = {.magnitude = tb_fmt_take_unsigned(spec, arguments), .base = 10};

    if (spec->conversion == 'o')
    {
        integer.base = 8;
    }
    if (spec->conversion == 'x' || spec->conversion == 'X')
    {
        integer.base = 16;
    }
    integer.uppercase = spec->conversion == 'X';
    tb_fmt_put_integer(output, spec, &integer);
}

/* ---------------------------------------------------------------------------
 * The other built-in conversions.
 * ------------------------------------------------------------------------- */

static void tb_fmt_convert_character(tb_fmt_output *output, const tb_fmt_spec *spec,
                                     va_list *arguments)
{
    const char byte = (char)(unsigned char)va_arg(*arguments, int);

    tb_fmt_put_padded(output, spec, &byte, 1);
}

static void tb_fmt_convert_string(tb_fmt_output *output, const tb_fmt_spec *spec,
                                  va_list *arguments)
{
    const char *string = va_arg(*arguments, const char *);
    size_t length = {0};

    if (string == NULL)
    {
        string = "(null)";
    }
    /* With a precision the string need not be terminated within it, so the
     * terminator is looked for only as far as the precision reaches. */
    while ((!spec->has_precision || length < spec->precision) && string[length] != '\0')
    {
        length++;
    }
    tb_fmt_put_padded(output, spec, string, length);
}

static void tb_fmt_convert_pointer(tb_fmt_output *output, const tb_fmt_spec *spec,
                                   va_list *arguments)
{
    const tb_fmt_integer integer = {
        .magnitude = (uintptr_t)va_arg(*arguments, void *), .base = 16, .forces_prefix = true};

    tb_fmt_put_integer(output, spec, &integer);
}

/* ---------------------------------------------------------------------------
 * Registered conversions.
 * ------------------------------------------------------------------------- */

static bool tb_fmt_is_letter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

/* Whether a character is free for tb_fmt_register(): a letter that C gives no
 * meaning in a conversion. */
static bool tb_fmt_is_registrable(char character)
{
    static const char taken[] = "diuxXocsp"
                                "aAeEfFgGn"
                                "hljztL";

    return tb_fmt_is_letter(character) &&
           tb_str_find_char(taken, 0, TB_STR_END, character) == TB_STR_NOT_FOUND;
}

tb_fmt_conversion *tb_fmt_register(char conversion, tb_fmt_conversion *function)
{
    tb_fmt_conversion *previous = NULL;

    TB_ASSERT_MSG(tb_fmt_is_registrable(conversion),
                  "the character must be a letter that is not already a conversion or a length");
    /* The registry has no place for any other character. */
    if (!tb_fmt_is_registrable(conversion))
    {
        return NULL;
    }
    previous = tb_fmt_registry[(unsigned char)conversion];
    tb_fmt_registry[(unsigned char)conversion] = function;
    return previous;
}

/* ---------------------------------------------------------------------------
 * Parsing a conversion.
 * ------------------------------------------------------------------------- */

static bool tb_fmt_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

/* Reads a decimal number at the cursor and moves the cursor past it. Returns
 * false when the number does not fit in size_t. */
static bool tb_fmt_parse_number(const char **cursor, size_t *number)
{
    size_t value = {0};

    while (tb_fmt_is_digit(**cursor))
    {
        if (!tb_size_multiply(value, 10, &value))
        {
            return false;
        }
        if (!tb_size_add(value, (size_t)(**cursor - '0'), &value))
        {
            return false;
        }
        (*cursor)++;
    }
    *number = value;
    return true;
}

static void tb_fmt_parse_flags(const char **cursor, tb_fmt_spec *spec)
{
    for (;;)
    {
        switch (**cursor)
        {
        case '-':
            spec->left_justify = true;
            break;
        case '+':
            spec->show_sign = true;
            break;
        case ' ':
            spec->space_sign = true;
            break;
        case '0':
            spec->zero_pad = true;
            break;
        case '#':
            spec->alternate = true;
            break;
        default:
            return;
        }
        (*cursor)++;
    }
}

/* A width given as an argument. A negative one asks for left justification
 * with its absolute value, as in C. */
static void tb_fmt_take_width(tb_fmt_spec *spec, va_list *arguments)
{
    const int width = va_arg(*arguments, int);

    spec->has_width = true;
    if (width < 0)
    {
        spec->left_justify = true;
        spec->width = (size_t)0 - (size_t)width;
        return;
    }
    spec->width = (size_t)width;
}

/* A precision given as an argument. A negative one is as if none were given. */
static void tb_fmt_take_precision(tb_fmt_spec *spec, va_list *arguments)
{
    const int precision = va_arg(*arguments, int);

    if (precision < 0)
    {
        return;
    }
    spec->has_precision = true;
    spec->precision = (size_t)precision;
}

static void tb_fmt_parse_length(const char **cursor, tb_fmt_spec *spec)
{
    switch (**cursor)
    {
    case 'h':
        spec->length = TB_FMT_LENGTH_SHORT;
        break;
    case 'l':
        spec->length = TB_FMT_LENGTH_LONG;
        break;
    case 'j':
        spec->length = TB_FMT_LENGTH_MAX;
        break;
    case 'z':
        spec->length = TB_FMT_LENGTH_SIZE;
        break;
    case 't':
        spec->length = TB_FMT_LENGTH_POINTER;
        break;
    default:
        return;
    }
    (*cursor)++;
    /* A doubled letter names the narrower or the wider type. */
    if (spec->length == TB_FMT_LENGTH_SHORT && **cursor == 'h')
    {
        spec->length = TB_FMT_LENGTH_CHAR;
        (*cursor)++;
    }
    if (spec->length == TB_FMT_LENGTH_LONG && **cursor == 'l')
    {
        spec->length = TB_FMT_LENGTH_LONG_LONG;
        (*cursor)++;
    }
}

/* Parses the conversion that starts after a '%' at the cursor and moves the
 * cursor to its conversion character, or to the terminator when the format
 * ends first. Returns false when a number in it does not fit. */
static bool tb_fmt_parse_spec(const char **cursor, tb_fmt_spec *spec, va_list *arguments)
{
    tb_fmt_parse_flags(cursor, spec);
    if (**cursor == '*')
    {
        tb_fmt_take_width(spec, arguments);
        (*cursor)++;
    }
    else if (tb_fmt_is_digit(**cursor))
    {
        spec->has_width = true;
        if (!tb_fmt_parse_number(cursor, &spec->width))
        {
            return false;
        }
    }
    if (**cursor == '.')
    {
        (*cursor)++;
        if (**cursor == '*')
        {
            tb_fmt_take_precision(spec, arguments);
            (*cursor)++;
        }
        else
        {
            /* A point with no digits is a precision of zero. */
            spec->has_precision = true;
            if (!tb_fmt_parse_number(cursor, &spec->precision))
            {
                return false;
            }
        }
    }
    tb_fmt_parse_length(cursor, spec);
    spec->conversion = **cursor;
    return true;
}

/* ---------------------------------------------------------------------------
 * Converting.
 * ------------------------------------------------------------------------- */

/* Runs the conversion a spec names. Returns false when there is none. */
static bool tb_fmt_convert(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const unsigned char code = (unsigned char)spec->conversion;
    /* Only the integer conversions have arguments of more than one size. */
    const bool has_length = spec->length != TB_FMT_LENGTH_DEFAULT;

    switch (spec->conversion)
    {
    case 'd':
    case 'i':
        tb_fmt_convert_signed(output, spec, arguments);
        return true;
    case 'u':
    case 'o':
    case 'x':
    case 'X':
        tb_fmt_convert_unsigned(output, spec, arguments);
        return true;
    default:
        break;
    }
    if (has_length)
    {
        return false;
    }
    switch (spec->conversion)
    {
    case 'c':
        tb_fmt_convert_character(output, spec, arguments);
        return true;
    case 's':
        tb_fmt_convert_string(output, spec, arguments);
        return true;
    case 'p':
        tb_fmt_convert_pointer(output, spec, arguments);
        return true;
    default:
        break;
    }
    if (code >= TB_FMT_REGISTRY_SIZE || tb_fmt_registry[code] == NULL)
    {
        return false;
    }
    tb_fmt_registry[code](output, spec, arguments);
    return true;
}

/* Formats to a sink, or only counts when the sink is NULL. Returns the number
 * of bytes of output. */
static size_t tb_fmt_format(tb_fmt_sink *sink, void *context, const char *format,
                            va_list *arguments)
{
    tb_fmt_output output = {.sink = sink, .context = context};
    const char *cursor = format;
    /* Start of the text that has been read and not yet written. */
    const char *pending = format;

    TB_ASSERT(format != NULL);
    while (*cursor != '\0')
    {
        tb_fmt_spec spec = {0};
        bool is_valid = false;

        if (*cursor != '%')
        {
            cursor++;
            continue;
        }
        tb_fmt_put(&output, pending, (size_t)(cursor - pending));
        pending = cursor;
        cursor++;
        if (*cursor == '%')
        {
            /* Skipping the first sign leaves the second to be written with
             * the text that follows it. */
            pending = cursor;
            cursor++;
            continue;
        }
        is_valid = tb_fmt_parse_spec(&cursor, &spec, arguments);
        if (is_valid)
        {
            is_valid = tb_fmt_convert(&output, &spec, arguments);
        }
        TB_ASSERT_MSG(is_valid, "the format string has an invalid conversion");
        if (*cursor != '\0')
        {
            cursor++;
        }
        /* An invalid conversion is left pending, so that it is written out
         * as it stands. */
        if (is_valid)
        {
            pending = cursor;
        }
    }
    tb_fmt_put(&output, pending, (size_t)(cursor - pending));
    return output.length;
}

/* ---------------------------------------------------------------------------
 * Formatting to a function.
 * ------------------------------------------------------------------------- */

size_t tb_fmt_vwrite(tb_fmt_sink *sink, void *context, const char *format, va_list arguments)
{
    va_list remaining = {0};
    size_t length = {0};

    TB_ASSERT(sink != NULL);
    /* The conversions take arguments through a pointer to the list. Where a
     * va_list is an array, a parameter of that type cannot be pointed to as
     * one, so the list is copied into a local first. */
    va_copy(remaining, arguments);
    length = tb_fmt_format(sink, context, format, &remaining);
    va_end(remaining);
    return length;
}

size_t tb_fmt_write(tb_fmt_sink *sink, void *context, const char *format, ...)
{
    va_list arguments = {0};
    size_t length = {0};

    va_start(arguments, format);
    length = tb_fmt_vwrite(sink, context, format, arguments);
    va_end(arguments);
    return length;
}

/* ---------------------------------------------------------------------------
 * Formatting into a buffer.
 * ------------------------------------------------------------------------- */

typedef struct tb_fmt_buffer_target
{
    char *bytes;     /* Where the output goes. */
    size_t capacity; /* Bytes of output that fit, terminator not counted. */
    size_t length;   /* Bytes written so far. */
} tb_fmt_buffer_target;

/* Copies what still fits and drops the rest. */
static void tb_fmt_buffer_sink(void *context, const char *bytes, size_t length)
{
    tb_fmt_buffer_target *const target = context;
    const size_t room = target->capacity - target->length;

    if (length > room)
    {
        length = room;
    }
    memcpy(target->bytes + target->length, bytes, length);
    target->length += length;
}

/* Number of bytes a format produces, without writing them anywhere. */
static size_t tb_fmt_measure(const char *format, va_list arguments)
{
    va_list remaining = {0};
    size_t length = {0};

    va_copy(remaining, arguments);
    length = tb_fmt_format(NULL, NULL, format, &remaining);
    va_end(remaining);
    return length;
}

size_t tb_fmt_vbuffer(char *buffer, size_t size, const char *format, va_list arguments)
{
    tb_fmt_buffer_target target = {.bytes = buffer};
    size_t length = {0};

    TB_ASSERT(buffer != NULL || size == 0);
    if (size == 0)
    {
        return tb_fmt_measure(format, arguments);
    }
    target.capacity = size - 1;
    length = tb_fmt_vwrite(tb_fmt_buffer_sink, &target, format, arguments);
    buffer[target.length] = '\0';
    return length;
}

size_t tb_fmt_buffer(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments = {0};
    size_t length = {0};

    va_start(arguments, format);
    length = tb_fmt_vbuffer(buffer, size, format, arguments);
    va_end(arguments);
    return length;
}

/* ---------------------------------------------------------------------------
 * Formatting into a new string.
 * ------------------------------------------------------------------------- */

char *tb_fmt_vstring(const tb_allocator *allocator, const char *format, va_list arguments)
{
    /* Formatted into a buffer on the stack, which also measures it. The
     * string is then exactly as long as its text, and can be released by its
     * length. */
    char local[TB_FMT_LOCAL_CAPACITY] = {0};
    const size_t length = tb_fmt_vbuffer(local, sizeof(local), format, arguments);
    char *string = NULL;
    size_t size = {0};

    if (!tb_size_add(length, 1, &size))
    {
        tb_memory_fail(1, length, 1);
    }
    string = tb_allocator_alloc(allocator, size, 1);
    if (length < sizeof(local))
    {
        /* All of it fitted, terminator included: it only has to be copied. */
        memcpy(string, local, size);
        return string;
    }
    /* Too long for the buffer, which held its beginning: formatted again,
     * into the string. */
    (void)tb_fmt_vbuffer(string, size, format, arguments);
    return string;
}

char *tb_fmt_string(const tb_allocator *allocator, const char *format, ...)
{
    va_list arguments = {0};
    char *string = NULL;

    va_start(arguments, format);
    string = tb_fmt_vstring(allocator, format, arguments);
    va_end(arguments);
    return string;
}

/* ---------------------------------------------------------------------------
 * Formatting to the standard streams.
 * ------------------------------------------------------------------------- */

static void tb_fmt_stream_sink(void *context, const char *bytes, size_t length)
{
    /* A stream that cannot be written to is the caller's to discover with
     * the stream functions; formatting has no failure to report. */
    (void)fwrite(bytes, 1, length, context);
}

size_t tb_fmt_vprint(const char *format, va_list arguments)
{
    return tb_fmt_vwrite(tb_fmt_stream_sink, stdout, format, arguments);
}

size_t tb_fmt_print(const char *format, ...)
{
    va_list arguments = {0};
    size_t length = {0};

    va_start(arguments, format);
    length = tb_fmt_vprint(format, arguments);
    va_end(arguments);
    return length;
}

size_t tb_fmt_vprint_error(const char *format, va_list arguments)
{
    return tb_fmt_vwrite(tb_fmt_stream_sink, stderr, format, arguments);
}

size_t tb_fmt_print_error(const char *format, ...)
{
    va_list arguments = {0};
    size_t length = {0};

    va_start(arguments, format);
    length = tb_fmt_vprint_error(format, arguments);
    va_end(arguments);
    return length;
}

bool tb_fmt_flush_error(void)
{
    return fflush(stderr) == 0 && ferror(stderr) == 0;
}

bool tb_fmt_flush(void)
{
    /* Both are flushed whatever the first reports. */
    const bool output_is_delivered = fflush(stdout) == 0 && ferror(stdout) == 0;
    const bool error_is_delivered = fflush(stderr) == 0 && ferror(stderr) == 0;

    return output_is_delivered && error_is_delivered;
}
