/* The floating-point conversions of the formatter: %f, %e, %g, and %a and
 * their capital forms. The digits come from the exact decimal value of the
 * double (decimal_internal.h), rounded to nearest with ties to even, so the
 * text is correctly rounded at every precision and the same on every
 * platform. */
#include "badger/assert.h"
#include "badger/fmt.h"
#include "decimal_internal.h"
#include "fmt_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C's precision when none is given, for %f, %e, and %g. */
#define FLOAT_DEFAULT_PRECISION 6
/* %g writes fixed notation when the exponent is at least this. */
#define FLOAT_LOWEST_FIXED_EXPONENT (-4)
/* Hexadecimal digits of the 52 fraction bits of a double. */
#define FLOAT_FRACTION_HEX_DIGITS 13u
#define FLOAT_FRACTION_BITS 52u
#define FLOAT_FRACTION_MASK ((UINT64_C(1) << FLOAT_FRACTION_BITS) - 1)
#define FLOAT_LOWEST_NORMAL_EXPONENT (-1022)
#define FLOAT_PADDING_BLOCK 32

/* Writes the text of a conversion, or only counts it when there is no
 * output, so that the padding can be known before anything is written. */
typedef struct float_writer
{
    tb_fmt_output *output;
    size_t length;
} float_writer;

static void float_put(float_writer *writer, const char *bytes, size_t length)
{
    if (writer->output != NULL)
    {
        tb_fmt_put(writer->output, bytes, length);
    }
    writer->length += length;
}

static void float_put_repeated(float_writer *writer, char byte, size_t count)
{
    char block[FLOAT_PADDING_BLOCK] = {0};
    size_t left = count;

    for (size_t index = 0; index < sizeof(block); index++)
    {
        block[index] = byte;
    }
    while (left > 0)
    {
        const size_t step = left < sizeof(block) ? left : sizeof(block);

        float_put(writer, block, step);
        left -= step;
    }
}

/* What follows the sign and any prefix: digits in one of the forms. */
typedef enum float_style
{
    FLOAT_STYLE_WORD,       /* inf or nan */
    FLOAT_STYLE_FIXED,      /* %f */
    FLOAT_STYLE_SCIENTIFIC, /* %e */
    FLOAT_STYLE_HEX         /* %a, after its 0x */
} float_style;

typedef struct float_body
{
    float_style style;
    bool uppercase;
    bool point_always;        /* The # flag: write the point with no digits after it. */
    size_t precision;         /* Digits after the point. */
    tb_decimal_exact decimal; /* For the decimal styles, already rounded. */
    const char *word;         /* For FLOAT_STYLE_WORD. */
    unsigned hex_lead;        /* For FLOAT_STYLE_HEX: the digit before the point, */
    uint64_t hex_fraction;    /* the digits after it, at most 13, */
    size_t hex_digits;        /* how many of them are in hex_fraction, */
    int hex_exponent;         /* and the power of two. */
} float_body;

static void float_put_exponent(float_writer *writer, char letter, int exponent,
                               size_t minimum_digits)
{
    char digits[12] = {0};
    size_t count = 0;
    unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
    const char sign = exponent < 0 ? '-' : '+';

    float_put(writer, &letter, 1);
    float_put(writer, &sign, 1);
    do
    {
        digits[sizeof(digits) - 1 - count] = (char)('0' + magnitude % 10);
        count++;
        magnitude /= 10;
    } while (magnitude != 0);
    float_put_repeated(writer, '0', minimum_digits > count ? minimum_digits - count : 0);
    float_put(writer, digits + sizeof(digits) - count, count);
}

static void float_put_point(float_writer *writer, const float_body *body)
{
    if (body->precision > 0 || body->point_always)
    {
        float_put(writer, ".", 1);
    }
}

/* %e: one digit, the point, `precision` digits, and an exponent of at least
 * two digits. */
static void float_put_scientific(float_writer *writer, const float_body *body)
{
    const tb_decimal_exact *const decimal = &body->decimal;
    const size_t after_first = decimal->count > 1 ? decimal->count - 1 : 0;
    const size_t written = after_first < body->precision ? after_first : body->precision;

    float_put(writer, decimal->count > 0 ? decimal->digits : "0", 1);
    float_put_point(writer, body);
    float_put(writer, decimal->digits + 1, written);
    float_put_repeated(writer, '0', body->precision - written);
    float_put_exponent(writer, body->uppercase ? 'E' : 'e',
                       decimal->count > 0 ? decimal->exponent : 0, 2);
}

/* %f: the integer part, the point, and `precision` digits. The digits were
 * rounded at the last place written, so all of them fall within it. */
static void float_put_fixed(float_writer *writer, const float_body *body)
{
    const tb_decimal_exact *const decimal = &body->decimal;
    const ptrdiff_t exponent = decimal->count > 0 ? decimal->exponent : -1;
    size_t leading_zeros = 0;
    size_t first_fraction = 0;
    size_t fraction_digits = 0;

    if (exponent < 0)
    {
        float_put(writer, "0", 1);
    }
    else
    {
        const size_t integer_digits = (size_t)exponent + 1;
        const size_t written = decimal->count < integer_digits ? decimal->count : integer_digits;

        float_put(writer, decimal->digits, written);
        float_put_repeated(writer, '0', integer_digits - written);
    }
    float_put_point(writer, body);
    if (decimal->count == 0)
    {
        float_put_repeated(writer, '0', body->precision);
        return;
    }
    if (exponent < -1)
    {
        leading_zeros = (size_t)(-exponent - 1);
        if (leading_zeros > body->precision)
        {
            leading_zeros = body->precision;
        }
    }
    first_fraction = exponent >= 0 ? (size_t)exponent + 1 : 0;
    if (decimal->count > first_fraction)
    {
        fraction_digits = decimal->count - first_fraction;
    }
    TB_ASSERT(leading_zeros + fraction_digits <= body->precision);
    float_put_repeated(writer, '0', leading_zeros);
    float_put(writer, decimal->digits + first_fraction, fraction_digits);
    float_put_repeated(writer, '0', body->precision - leading_zeros - fraction_digits);
}

/* %a, after the 0x: a digit, the point, the hexadecimal digits, and a binary
 * exponent in decimal. */
static void float_put_hex(float_writer *writer, const float_body *body)
{
    static const char lowercase[] = "0123456789abcdef";
    static const char uppercase[] = "0123456789ABCDEF";
    const char *const alphabet = body->uppercase ? uppercase : lowercase;
    char digits[FLOAT_FRACTION_HEX_DIGITS] = {0};

    float_put(writer, &alphabet[body->hex_lead], 1);
    float_put_point(writer, body);
    for (size_t index = 0; index < body->hex_digits; index++)
    {
        const unsigned shift = (unsigned)(4 * (body->hex_digits - 1 - index));

        digits[index] = alphabet[(body->hex_fraction >> shift) & 0xFu];
    }
    float_put(writer, digits, body->hex_digits);
    float_put_repeated(writer, '0', body->precision - body->hex_digits);
    float_put_exponent(writer, body->uppercase ? 'P' : 'p', body->hex_exponent, 1);
}

static void float_put_body(float_writer *writer, const float_body *body)
{
    switch (body->style)
    {
    case FLOAT_STYLE_WORD:
        float_put(writer, body->word, 3);
        break;
    case FLOAT_STYLE_FIXED:
        float_put_fixed(writer, body);
        break;
    case FLOAT_STYLE_SCIENTIFIC:
        float_put_scientific(writer, body);
        break;
    case FLOAT_STYLE_HEX:
        float_put_hex(writer, body);
        break;
    }
}

/* --- choosing the digits ---------------------------------------------------------------- */

static size_t float_precision(const tb_fmt_spec *spec)
{
    return spec->has_precision ? spec->precision : FLOAT_DEFAULT_PRECISION;
}

static void float_prepare_scientific(float_body *body, const tb_decimal_parts *parts,
                                     size_t precision)
{
    tb_decimal_exact_of(parts->mantissa, parts->exponent, &body->decimal);
    tb_decimal_round(&body->decimal, (ptrdiff_t)precision + 1);
    body->style = FLOAT_STYLE_SCIENTIFIC;
    body->precision = precision;
}

static void float_prepare_fixed(float_body *body, const tb_decimal_parts *parts, size_t precision)
{
    tb_decimal_exact_of(parts->mantissa, parts->exponent, &body->decimal);
    if (body->decimal.count > 0)
    {
        tb_decimal_round(&body->decimal,
                         (ptrdiff_t)body->decimal.exponent + 1 + (ptrdiff_t)precision);
    }
    body->style = FLOAT_STYLE_FIXED;
    body->precision = precision;
}

/* %g: P significant digits, in fixed notation when the exponent X of the
 * rounded value is from -4 to P - 1, and in scientific notation otherwise.
 * Without the # flag, zeros at the end of the fraction are dropped, and the
 * point with them. */
static void float_prepare_general(float_body *body, const tb_decimal_parts *parts, size_t precision,
                                  bool keeps_zeros)
{
    const size_t significant = precision == 0 ? 1 : precision;
    ptrdiff_t exponent = 0;
    size_t written = 0;

    tb_decimal_exact_of(parts->mantissa, parts->exponent, &body->decimal);
    tb_decimal_round(&body->decimal, (ptrdiff_t)significant);
    exponent = body->decimal.count > 0 ? body->decimal.exponent : 0;
    /* The digits that remain after rounding, beyond the first. */
    written = body->decimal.count > 1 ? body->decimal.count - 1 : 0;
    if ((ptrdiff_t)significant > exponent && exponent >= FLOAT_LOWEST_FIXED_EXPONENT)
    {
        const ptrdiff_t after_point = (ptrdiff_t)significant - 1 - exponent;
        const ptrdiff_t needed = (ptrdiff_t)written - exponent;

        body->style = FLOAT_STYLE_FIXED;
        body->precision = (size_t)after_point;
        if (!keeps_zeros)
        {
            body->precision = needed > 0 ? (size_t)needed : 0;
        }
        return;
    }
    body->style = FLOAT_STYLE_SCIENTIFIC;
    body->precision = keeps_zeros ? significant - 1 : written;
}

/* %a: exact, or rounded to `precision` hexadecimal digits, ties to even. */
static void float_prepare_hex(float_body *body, const tb_decimal_parts *parts,
                              const tb_fmt_spec *spec)
{
    uint64_t fraction = 0;

    body->style = FLOAT_STYLE_HEX;
    if (parts->mantissa == 0)
    {
        body->hex_lead = 0;
        body->hex_exponent = 0;
    }
    else if (parts->is_normal)
    {
        body->hex_lead = 1;
        fraction = parts->mantissa & FLOAT_FRACTION_MASK;
        body->hex_exponent = parts->exponent + (int)FLOAT_FRACTION_BITS;
    }
    else
    {
        body->hex_lead = 0;
        fraction = parts->mantissa;
        body->hex_exponent = FLOAT_LOWEST_NORMAL_EXPONENT;
    }

    if (!spec->has_precision)
    {
        /* As many digits as the value needs, and no more. */
        size_t digits = FLOAT_FRACTION_HEX_DIGITS;

        while (digits > 0 && (fraction & 0xFu) == 0)
        {
            fraction >>= 4;
            digits--;
        }
        body->hex_fraction = fraction;
        body->hex_digits = digits;
        body->precision = digits;
        return;
    }
    body->precision = spec->precision;
    if (spec->precision >= FLOAT_FRACTION_HEX_DIGITS)
    {
        body->hex_fraction = fraction;
        body->hex_digits = FLOAT_FRACTION_HEX_DIGITS;
        return;
    }
    {
        /* The lead digit and the kept digits form one number, so that a
         * carry out of the digits reaches the lead digit, and a tie looks at
         * the parity of the last digit kept, whichever that is. */
        const unsigned kept_bits = (unsigned)(4 * spec->precision);
        const unsigned dropped_bits = FLOAT_FRACTION_BITS - kept_bits;
        const uint64_t dropped = fraction & ((UINT64_C(1) << dropped_bits) - 1);
        const uint64_t half = UINT64_C(1) << (dropped_bits - 1);
        uint64_t kept = ((uint64_t)body->hex_lead << kept_bits) | (fraction >> dropped_bits);

        if (dropped > half || (dropped == half && (kept % 2) != 0))
        {
            kept++;
        }
        body->hex_lead = (unsigned)(kept >> kept_bits);
        body->hex_fraction = kept & ((UINT64_C(1) << kept_bits) - 1);
        body->hex_digits = spec->precision;
    }
}

static char float_sign(const tb_fmt_spec *spec, const tb_decimal_parts *parts)
{
    /* NaN has no sign: the same text whatever its sign bit. */
    if (parts->negative && !parts->is_nan)
    {
        return '-';
    }
    if (spec->show_sign)
    {
        return '+';
    }
    if (spec->space_sign)
    {
        return ' ';
    }
    return '\0';
}

void tb_fmt_convert_float(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const double value = va_arg(*arguments, double);
    const tb_decimal_parts parts = tb_decimal_split(value);
    const char conversion = spec->conversion;
    const bool uppercase = conversion >= 'A' && conversion <= 'Z';
    const char lower = (char)(uppercase ? conversion - 'A' + 'a' : conversion);
    const char sign = float_sign(spec, &parts);
    float_body body = {.uppercase = uppercase, .point_always = spec->alternate};
    float_writer counter = {0};
    float_writer writer = {.output = output};
    const char *prefix = "";
    size_t prefix_length = 0;
    size_t padding = 0;
    bool pads_with_zeros = spec->zero_pad && !spec->left_justify;

    if (parts.is_infinite || parts.is_nan)
    {
        body.style = FLOAT_STYLE_WORD;
        body.word = parts.is_nan ? (uppercase ? "NAN" : "nan") : (uppercase ? "INF" : "inf");
        pads_with_zeros = false;
    }
    else if (lower == 'e')
    {
        float_prepare_scientific(&body, &parts, float_precision(spec));
    }
    else if (lower == 'f')
    {
        float_prepare_fixed(&body, &parts, float_precision(spec));
    }
    else if (lower == 'g')
    {
        float_prepare_general(&body, &parts, float_precision(spec), spec->alternate);
    }
    else
    {
        TB_ASSERT(lower == 'a');
        float_prepare_hex(&body, &parts, spec);
        prefix = uppercase ? "0X" : "0x";
        prefix_length = 2;
    }

    float_put_body(&counter, &body);
    counter.length += prefix_length + (sign != '\0' ? 1u : 0u);
    if (spec->has_width && spec->width > counter.length)
    {
        padding = spec->width - counter.length;
    }
    if (!spec->left_justify && !pads_with_zeros)
    {
        float_put_repeated(&writer, ' ', padding);
    }
    if (sign != '\0')
    {
        float_put(&writer, &sign, 1);
    }
    float_put(&writer, prefix, prefix_length);
    if (pads_with_zeros)
    {
        float_put_repeated(&writer, '0', padding);
    }
    float_put_body(&writer, &body);
    if (spec->left_justify)
    {
        float_put_repeated(&writer, ' ', padding);
    }
}
