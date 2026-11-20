#include "badger/decimal.h"

#include "badger/assert.h"
#include "badger/xp.h"
#include "decimal_internal.h"
#include "decimal_table.h"

#include <stdint.h>
#include <string.h>

/* The fields of an IEEE 754 binary64 value. */
#define DECIMAL_FRACTION_BITS 52
#define DECIMAL_FRACTION_MASK ((UINT64_C(1) << DECIMAL_FRACTION_BITS) - 1)
#define DECIMAL_HIDDEN_BIT (UINT64_C(1) << DECIMAL_FRACTION_BITS)
#define DECIMAL_EXPONENT_MASK UINT64_C(0x7FF)
#define DECIMAL_SIGN_BIT (UINT64_C(1) << 63)
#define DECIMAL_EXPONENT_BIAS 1023
/* The exponent of the lowest bit of a subnormal value, and of every value:
 * the smallest positive double is 2^-1074. */
#define DECIMAL_LOWEST_BIT_EXPONENT (-1074)
/* The biased exponent of infinity and NaN. */
#define DECIMAL_SPECIAL_EXPONENT 2047
#define DECIMAL_INFINITY_BITS (DECIMAL_EXPONENT_MASK << DECIMAL_FRACTION_BITS)
#define DECIMAL_QUIET_NAN_BITS (DECIMAL_INFINITY_BITS | (UINT64_C(1) << 51))

/* Digits of work for the exact value of a double: 56 bits times 5^1077,
 * about 2557 bits, is 80 digits of 32 bits. */
#define DECIMAL_EXACT_WORK_DIGITS ((size_t)96)
#define DECIMAL_EXACT_TEXT_SIZE ((size_t)1024)
/* 5^13, the largest power of five that fits in a digit of 32 bits. */
#define DECIMAL_FIVE_TO_THE_13 1220703125u
#define DECIMAL_FIVE_STEP 13u
/* 10^9, the largest power of ten that fits in a digit of 32 bits. */
#define DECIMAL_TEN_TO_THE_9 1000000000u
#define DECIMAL_TEN_STEP 9u

/* Seventeen significant digits tell any two doubles apart. */
#define DECIMAL_MAXIMUM_SHORTEST_DIGITS 17

/* Python's repr writes fixed notation for decimal exponents from -4 to 15. */
#define DECIMAL_FIXED_LOWEST_EXPONENT (-4)
#define DECIMAL_FIXED_LIMIT_EXPONENT 16

/* Reading: the significant digits that are kept. A halfway point between two
 * doubles has at most 769 significant digits, so a text with more is the
 * kept digits and one nonzero digit after them, which sits on the same side
 * of every halfway point as the whole text. */
#define DECIMAL_READ_DIGITS ((size_t)800)
/* An exponent written beyond this is read as this. No text that fits in
 * memory has enough digits to bring such a value back into range, and the
 * arithmetic on exponents stays far from the limits of 64 bits. */
#define DECIMAL_EXPONENT_SATURATION INT64_C(100000000000000000)
/* A decimal value at or above 10^310 is infinite; below 10^-324, zero. */
#define DECIMAL_LARGEST_DECIMAL_EXPONENT 310
#define DECIMAL_SMALLEST_DECIMAL_EXPONENT (-324)
/* Big integers of reading: the numerator after its shift, and the power of
 * ten that divides it. */
#define DECIMAL_NUMERATOR_DIGITS ((size_t)256)
#define DECIMAL_DENOMINATOR_DIGITS ((size_t)128)
/* The quotient of reading keeps a few bits more than the 53 of a double, so
 * that the bits below them only say whether anything is left over. */
#define DECIMAL_QUOTIENT_BITS 56
/* Hexadecimal digits of a mantissa that fit in 64 bits with room to spare. */
#define DECIMAL_HEX_DIGITS 15
/* A binary exponent beyond these makes any 60-bit mantissa infinite, or
 * zero. */
#define DECIMAL_HEX_LARGEST_EXPONENT 1100
#define DECIMAL_HEX_SMALLEST_EXPONENT (-1200)

const char *tb_decimal_result_name(tb_decimal_result result)
{
    switch (result)
    {
    case TB_DECIMAL_OK:
        return "ok";
    case TB_DECIMAL_INVALID:
        return "invalid argument";
    case TB_DECIMAL_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown result";
    }
}

/* --- the parts of a double ----------------------------------------------------------- */

static uint64_t decimal_bits_of(double value)
{
    uint64_t bits = 0;

    TB_STATIC_ASSERT(sizeof(double) == sizeof(uint64_t), "a double has 64 bits");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double decimal_value_of(uint64_t bits)
{
    double value = 0.0;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

tb_decimal_parts tb_decimal_split(double value)
{
    const uint64_t bits = decimal_bits_of(value);
    const uint64_t fraction = bits & DECIMAL_FRACTION_MASK;
    const int biased = (int)((bits >> DECIMAL_FRACTION_BITS) & DECIMAL_EXPONENT_MASK);
    tb_decimal_parts parts = {.negative = (bits & DECIMAL_SIGN_BIT) != 0};

    if (biased == DECIMAL_SPECIAL_EXPONENT)
    {
        parts.is_infinite = fraction == 0;
        parts.is_nan = fraction != 0;
        return parts;
    }
    if (biased == 0)
    {
        parts.mantissa = fraction;
        parts.exponent = DECIMAL_LOWEST_BIT_EXPONENT;
        return parts;
    }
    parts.mantissa = fraction | DECIMAL_HIDDEN_BIT;
    parts.exponent = biased - DECIMAL_EXPONENT_BIAS - DECIMAL_FRACTION_BITS;
    parts.is_normal = true;
    return parts;
}

/* --- exact digits ----------------------------------------------------------------------- */

/* z = z * factor over a length that grows by the carry. */
static void decimal_multiply_digit(tb_xp_digit *number, size_t *length, size_t capacity,
                                   tb_xp_digit factor)
{
    const tb_xp_digit carry = tb_xp_product(*length, number, number, factor);

    if (carry != 0)
    {
        TB_ASSERT(*length < capacity);
        number[*length] = carry;
        (*length)++;
    }
}

/* z = z * base^count, in steps of the largest power of the base in a digit. */
static void decimal_multiply_power(tb_xp_digit *number, size_t *length, size_t capacity,
                                   unsigned base, size_t count, tb_xp_digit step_power, size_t step)
{
    size_t remaining = count;
    tb_xp_digit last = 1;

    while (remaining >= step)
    {
        decimal_multiply_digit(number, length, capacity, step_power);
        remaining -= step;
    }
    for (size_t index = 0; index < remaining; index++)
    {
        last *= base;
    }
    decimal_multiply_digit(number, length, capacity, last);
}

void tb_decimal_exact_of(uint64_t mantissa, int exponent, tb_decimal_exact *exact)
{
    tb_xp_digit number[DECIMAL_EXACT_WORK_DIGITS] = {0};
    tb_xp_digit scratch[DECIMAL_EXACT_WORK_DIGITS] = {0};
    char text[DECIMAL_EXACT_TEXT_SIZE] = {0};
    size_t length = 2;
    size_t point = 0;
    size_t required = 0;
    size_t text_length = 0;
    tb_xp_result written = TB_XP_OK;

    TB_ASSERT(exact != NULL);
    exact->count = 0;
    exact->exponent = 0;
    if (mantissa == 0)
    {
        return;
    }
    (void)tb_xp_from_u64(length, number, mantissa);
    if (exponent >= 0)
    {
        length = DECIMAL_EXACT_WORK_DIGITS;
        tb_xp_shift_left(length, number, number, (size_t)exponent);
    }
    else
    {
        /* m * 2^-k = m * 5^k / 10^k: the digits of m * 5^k, with the point k
         * places from the right. */
        point = (size_t)-exponent;
        decimal_multiply_power(number, &length, DECIMAL_EXACT_WORK_DIGITS, 5, point,
                               DECIMAL_FIVE_TO_THE_13, DECIMAL_FIVE_STEP);
    }
    length = tb_xp_length(length, number);
    written = tb_xp_to_string(length, number, 10, scratch, text, sizeof(text), &required);
    TB_ASSERT(written == TB_XP_OK);
    text_length = required - 1;
    TB_ASSERT(text_length <= TB_DECIMAL_EXACT_CAPACITY);
    exact->exponent = (int)text_length - 1 - (int)point;
    while (text[text_length - 1] == '0')
    {
        text_length--;
    }
    memcpy(exact->digits, text, text_length);
    exact->count = text_length;
}

static void decimal_strip_trailing_zeros(tb_decimal_exact *exact)
{
    while (exact->count > 0 && exact->digits[exact->count - 1] == '0')
    {
        exact->count--;
    }
    if (exact->count == 0)
    {
        exact->exponent = 0;
    }
}

/* Keeps the first `kept` digits and adds one to the last of them, carrying.
 * With no digit kept, the result is one unit of the place above the first. */
static void decimal_increment(tb_decimal_exact *exact, size_t kept)
{
    size_t position = kept;

    while (position > 0 && exact->digits[position - 1] == '9')
    {
        position--;
    }
    if (position == 0)
    {
        exact->digits[0] = '1';
        exact->count = 1;
        exact->exponent++;
        return;
    }
    exact->digits[position - 1]++;
    exact->count = position;
}

void tb_decimal_round(tb_decimal_exact *exact, ptrdiff_t significant)
{
    size_t kept = 0;
    char next = '0';
    bool rounds_up = false;

    TB_ASSERT(exact != NULL);
    if (exact->count == 0 || significant >= (ptrdiff_t)exact->count)
    {
        return;
    }
    if (significant < 0)
    {
        exact->count = 0;
        exact->exponent = 0;
        return;
    }
    kept = (size_t)significant;
    next = exact->digits[kept];
    if (next != '5')
    {
        rounds_up = next > '5';
    }
    else if (kept + 1 < exact->count)
    {
        /* The last digit is never '0', so anything after the 5 is more. */
        rounds_up = true;
    }
    else
    {
        /* Exactly halfway: to the even neighbor. No kept digit is even. */
        rounds_up = kept > 0 && ((exact->digits[kept - 1] - '0') % 2) != 0;
    }
    if (rounds_up)
    {
        decimal_increment(exact, kept);
        return;
    }
    exact->count = kept;
    decimal_strip_trailing_zeros(exact);
}

char tb_decimal_digit_at(const tb_decimal_exact *exact, ptrdiff_t place)
{
    const ptrdiff_t index = (ptrdiff_t)exact->exponent - place;

    if (index < 0 || index >= (ptrdiff_t)exact->count)
    {
        return '0';
    }
    return exact->digits[index];
}

static int decimal_compare(const tb_decimal_exact *left, const tb_decimal_exact *right)
{
    const size_t longest = left->count > right->count ? left->count : right->count;

    if (left->count == 0 || right->count == 0)
    {
        return (left->count != 0 ? 1 : 0) - (right->count != 0 ? 1 : 0);
    }
    if (left->exponent != right->exponent)
    {
        return left->exponent < right->exponent ? -1 : 1;
    }
    for (size_t index = 0; index < longest; index++)
    {
        const char left_digit = index < left->count ? left->digits[index] : '0';
        const char right_digit = index < right->count ? right->digits[index] : '0';

        if (left_digit != right_digit)
        {
            return left_digit < right_digit ? -1 : 1;
        }
    }
    return 0;
}

/* --- the shortest text ------------------------------------------------------------------ */

/* Whether a candidate lies within the bounds of the values that read as the
 * double, including the bounds when the double's mantissa is even. */
static bool decimal_is_between(const tb_decimal_exact *candidate, const tb_decimal_exact *low,
                               const tb_decimal_exact *high, bool includes_bounds)
{
    const int above_low = decimal_compare(candidate, low);
    const int below_high = decimal_compare(candidate, high);

    return (above_low > 0 || (includes_bounds && above_low == 0)) &&
           (below_high < 0 || (includes_bounds && below_high == 0));
}

/* The fewest significant digits that read back as the value, and of those
 * the nearest to it, ties to even, found by trying each count of digits in
 * exact arithmetic. It is the definition that the fast conversion below is
 * checked against, by the engine's tests. The values that read as the double are
 * those between the halfway points to its neighbors. For each count of
 * digits, the only candidates are the value cut to that many digits and the
 * next number of that many digits above it, since the range holds the value
 * and so holds one of them if it holds any number of that many digits. */
void tb_decimal_shortest_exact(const tb_decimal_parts *parts, tb_decimal_exact *result)
{
    tb_decimal_exact value = {0};
    tb_decimal_exact low = {0};
    tb_decimal_exact high = {0};
    const uint64_t mantissa = parts->mantissa;
    const bool includes_bounds = (mantissa % 2) == 0;

    tb_decimal_exact_of(mantissa, parts->exponent, &value);
    tb_decimal_exact_of(2 * mantissa + 1, parts->exponent - 1, &high);
    /* Below a power of two the gap to the next smaller double is half as
     * wide, unless that double is subnormal. */
    if (parts->is_normal && mantissa == DECIMAL_HIDDEN_BIT &&
        parts->exponent > DECIMAL_LOWEST_BIT_EXPONENT)
    {
        tb_decimal_exact_of(4 * mantissa - 1, parts->exponent - 2, &low);
    }
    else
    {
        tb_decimal_exact_of(2 * mantissa - 1, parts->exponent - 1, &low);
    }

    for (size_t digits = 1; digits < DECIMAL_MAXIMUM_SHORTEST_DIGITS; digits++)
    {
        bool down_reads_back = false;
        bool up_reads_back = false;

        if (value.count <= digits)
        {
            break;
        }
        *result = value;
        result->count = digits;
        decimal_strip_trailing_zeros(result);
        down_reads_back = decimal_is_between(result, &low, &high, includes_bounds);
        *result = value;
        decimal_increment(result, digits);
        up_reads_back = decimal_is_between(result, &low, &high, includes_bounds);
        if (down_reads_back && up_reads_back)
        {
            *result = value;
            tb_decimal_round(result, (ptrdiff_t)digits);
            return;
        }
        if (up_reads_back)
        {
            return;
        }
        if (down_reads_back)
        {
            *result = value;
            result->count = digits;
            decimal_strip_trailing_zeros(result);
            return;
        }
    }
    *result = value;
    tb_decimal_round(result, DECIMAL_MAXIMUM_SHORTEST_DIGITS);
}

/* --- the shortest digits, fast: Schubfach ---------------------------------------------- */

/* Raffaello Giulietti, "The Schubfach way to render doubles" (2020). The
 * values that read as the double lie in an interval whose width is the
 * double's spacing, 2^q. With k = floor(log10(2^q)), the interval holds at
 * most one multiple of 10^(k+1), and at least one of 10^k: the shortest
 * decimal is the one multiple of 10^(k+1) inside it, if there is one, and
 * otherwise the multiple of 10^k nearest the value, ties to even. Each is
 * found by multiplying the value and the interval's ends by 10^-k, rounded
 * to odd from a product of 64 by 128 bits, which keeps every comparison
 * exact. */

/* The 128 bits of a product of two 64-bit numbers. */
typedef struct decimal_u128
{
    uint64_t high;
    uint64_t low;
} decimal_u128;

static decimal_u128 decimal_multiply_64(uint64_t left, uint64_t right)
{
    const uint64_t left_low = left & UINT32_MAX;
    const uint64_t left_high = left >> 32;
    const uint64_t right_low = right & UINT32_MAX;
    const uint64_t right_high = right >> 32;
    const uint64_t low_low = left_low * right_low;
    const uint64_t high_low = left_high * right_low;
    const uint64_t low_high = left_low * right_high;
    const uint64_t high_high = left_high * right_high;
    /* No sum of 32-bit halves here can carry out of 64 bits. */
    const uint64_t middle = (low_low >> 32) + (high_low & UINT32_MAX) + low_high;
    decimal_u128 product = {0};

    product.high = high_high + (high_low >> 32) + (middle >> 32);
    product.low = (middle << 32) | (low_low & UINT32_MAX);
    return product;
}

/* floor(value / 2^shift), rounding toward negative infinity for a negative
 * value, which C's shift of a signed number does not promise. */
static int32_t decimal_floor_shift(int32_t value, unsigned shift)
{
    if (value >= 0)
    {
        return value >> shift;
    }
    return -(int32_t)((((uint32_t)-value) + ((UINT32_C(1) << shift) - 1u)) >> shift);
}

/* floor(log10(2^exponent)), and floor(log10(3/4 * 2^exponent)), for
 * exponents from -1500 to 1500, and floor(log2(10^exponent)) for exponents
 * from -400 to 400: fixed-point approximations exact over those ranges. */
static int32_t decimal_floor_log10_pow2(int32_t exponent, bool three_quarters)
{
    return decimal_floor_shift(exponent * 1262611 - (three_quarters ? 524031 : 0), 22);
}

static int32_t decimal_floor_log2_pow10(int32_t exponent)
{
    return decimal_floor_shift(exponent * 1741647, 19);
}

/* floor(g * value / 2^128), with its lowest bit set when anything was cut
 * off. g is 10^k scaled into [2^127, 2^128) and rounded up, which errs by
 * less than one unit of the 64 bits below the result. */
static uint64_t decimal_round_to_odd(const uint64_t g[2], uint64_t value)
{
    const decimal_u128 low = decimal_multiply_64(g[1], value);
    const decimal_u128 high = decimal_multiply_64(g[0], value);
    const uint64_t middle = high.low + low.high;
    const uint64_t top = high.high + (middle < high.low ? 1u : 0u);

    return top | (middle > 1 ? 1u : 0u);
}

/* The digits of `value` times 10^exponent, its trailing zeros removed. */
static void decimal_from_integer(uint64_t value, int exponent, tb_decimal_exact *result)
{
    char text[DECIMAL_MAXIMUM_SHORTEST_DIGITS + 4] = {0};
    size_t count = 0;

    TB_ASSERT(value != 0);
    while (value % 10u == 0)
    {
        value /= 10u;
        exponent++;
    }
    while (value != 0)
    {
        text[count] = (char)('0' + (int)(value % 10u));
        count++;
        value /= 10u;
    }
    for (size_t index = 0; index < count; index++)
    {
        result->digits[index] = text[count - 1 - index];
    }
    result->count = count;
    result->exponent = exponent + (int)count - 1;
}

void tb_decimal_shortest_fast(const tb_decimal_parts *parts, tb_decimal_exact *result)
{
    const uint64_t c = parts->mantissa;
    const int32_t q = parts->exponent;
    const bool includes_bounds = (c % 2u) == 0;
    /* Below a power of two the gap to the next smaller double is half as
     * wide, unless that double is subnormal. */
    const bool lower_is_closer =
        parts->is_normal && c == DECIMAL_HIDDEN_BIT && q > DECIMAL_LOWEST_BIT_EXPONENT;
    int32_t k = 0;
    int32_t h = 0;
    const uint64_t *g = NULL;
    uint64_t lower = 0;
    uint64_t middle = 0;
    uint64_t upper = 0;
    uint64_t s = 0;

    TB_ASSERT(c != 0);
    /* An integer below 2^53 is its own shortest decimal. */
    if (q <= 0 && q > -DECIMAL_FRACTION_BITS - 1 && (c & ((UINT64_C(1) << -q) - 1u)) == 0)
    {
        decimal_from_integer(c >> -q, 0, result);
        return;
    }
    k = decimal_floor_log10_pow2(q, lower_is_closer);
    h = q + decimal_floor_log2_pow10(-k) + 1;
    TB_ASSERT(-k >= TB_DECIMAL_TABLE_LOWEST && -k <= TB_DECIMAL_TABLE_HIGHEST);
    TB_ASSERT(h >= 1 && h <= 4);
    g = tb_decimal_table[-k - TB_DECIMAL_TABLE_LOWEST];
    /* The value and the interval's ends, in units of 2^(q-2), times 10^-k. */
    lower = decimal_round_to_odd(g, (4u * c - 2u + (lower_is_closer ? 1u : 0u)) << h);
    middle = decimal_round_to_odd(g, (4u * c) << h);
    upper = decimal_round_to_odd(g, (4u * c + 2u) << h);
    if (!includes_bounds)
    {
        lower++;
        upper--;
    }
    s = middle / 4u;
    if (s >= 10u)
    {
        const uint64_t tens = s / 10u;
        const bool down_inside = lower <= 40u * tens;
        const bool up_inside = 40u * tens + 40u <= upper;

        if (down_inside != up_inside)
        {
            decimal_from_integer(tens + (up_inside ? 1u : 0u), k + 1, result);
            return;
        }
    }
    {
        const bool down_inside = lower <= 4u * s;
        const bool up_inside = 4u * s + 4u <= upper;
        if (down_inside != up_inside)
        {
            decimal_from_integer(s + (up_inside ? 1u : 0u), k, result);
            return;
        }
    }
    /* Both are inside: the nearer, ties to even. */
    {
        const uint64_t halfway = 4u * s + 2u;
        const bool round_up = middle > halfway || (middle == halfway && (s & 1u) != 0);
        decimal_from_integer(s + (round_up ? 1u : 0u), k, result);
    }
}

/* --- a fixed number of digits, and reading, fast ----------------------------------------- */

/* Both multiply by a power of ten from the table above, and both must round
 * a product that may err. An entry of the table is 10^t rounded up, so the
 * product of a number n with it is at most n above the true product, and
 * exact for 10^0 to 10^55, which 128 bits hold exactly (10^t = 5^t * 2^t,
 * and 5^55 < 2^128). Where the error leaves the rounding open, as at a tie
 * the product cannot see, the fast form gives up, and the exact form, in
 * big integers, decides. */

/* The highest power of ten that the table holds exactly. */
#define DECIMAL_TABLE_EXACT_HIGHEST 55

/* The most significant digits rounded fast: the rounded number, and ten
 * times it while the exponent is being found, fit in 64 bits. */
#define DECIMAL_FAST_SIGNIFICANT_MAX ((size_t)17)

/* The most decimal digits read fast: 10^19 < 2^64. */
#define DECIMAL_FAST_READ_DIGITS ((size_t)19)

/* Bits of a product of 64 by 128 bits. */
#define DECIMAL_PRODUCT_BITS 192

/* A number of 192 bits, lowest word first. */
typedef struct decimal_u192
{
    uint64_t word[3];
} decimal_u192;

/* `value` times the table's entry g, high word first. */
static decimal_u192 decimal_multiply_entry(uint64_t value, const uint64_t g[2])
{
    const decimal_u128 low = decimal_multiply_64(value, g[1]);
    const decimal_u128 high = decimal_multiply_64(value, g[0]);
    decimal_u192 product = {{0}};

    product.word[0] = low.low;
    product.word[1] = low.high + high.low;
    product.word[2] = high.high + (product.word[1] < low.high ? 1u : 0u);
    return product;
}

static bool decimal_u192_bit(const decimal_u192 *number, unsigned bit)
{
    return ((number->word[bit / 64u] >> (bit % 64u)) & 1u) != 0;
}

/* Whether any bit at or above `bit` is set. */
static bool decimal_u192_any_from(const decimal_u192 *number, unsigned bit)
{
    unsigned word = bit / 64u;

    if (bit >= DECIMAL_PRODUCT_BITS)
    {
        return false;
    }
    if ((number->word[word] >> (bit % 64u)) != 0)
    {
        return true;
    }
    for (word++; word < 3u; word++)
    {
        if (number->word[word] != 0)
        {
            return true;
        }
    }
    return false;
}

/* The 64 bits from `bit` up. */
static uint64_t decimal_u192_bits_from(const decimal_u192 *number, unsigned bit)
{
    const unsigned word = bit / 64u;
    const unsigned offset = bit % 64u;
    uint64_t bits = 0;

    if (bit >= DECIMAL_PRODUCT_BITS)
    {
        return 0;
    }
    bits = number->word[word] >> offset;
    if (offset != 0 && word + 1u < 3u)
    {
        bits |= number->word[word + 1u] << (64u - offset);
    }
    return bits;
}

/* Whether the number formed by the bits below `bit` is at least `value`. */
static bool decimal_u192_low_at_least(const decimal_u192 *number, unsigned bit, uint64_t value)
{
    uint64_t lowest = number->word[0];

    if (bit > 64u)
    {
        /* Any bit from 64 up to `bit` makes it larger than any 64-bit value. */
        for (unsigned word = 1; word * 64u < bit; word++)
        {
            const unsigned width = bit - word * 64u;
            const uint64_t mask = width >= 64u ? UINT64_MAX : (UINT64_C(1) << width) - 1u;

            if ((number->word[word] & mask) != 0)
            {
                return true;
            }
        }
    }
    else if (bit < 64u)
    {
        lowest &= (UINT64_C(1) << bit) - 1u;
    }
    return lowest >= value;
}

/* How a product p, at most `error` above the true one, rounds at bit
 * `shift`: whether the true product's part below that bit is less than
 * half of it, more, exactly half, or too close to half to say. A true part
 * that is a little below zero, which the error allows, is a part close to
 * the whole of the place below, which rounds up to the same result as a
 * part close to zero rounds down to. */
typedef enum decimal_rounding
{
    DECIMAL_ROUND_DOWN,
    DECIMAL_ROUND_UP,
    DECIMAL_ROUND_TIE,
    DECIMAL_ROUND_UNSURE
} decimal_rounding;

static decimal_rounding decimal_rounding_of(const decimal_u192 *product, unsigned shift,
                                            uint64_t error)
{
    TB_ASSERT(shift >= 1 && shift <= DECIMAL_PRODUCT_BITS);
    if (!decimal_u192_bit(product, shift - 1u))
    {
        return DECIMAL_ROUND_DOWN;
    }
    /* At least half: by how much is the part below the half bit. */
    if (error == 0)
    {
        return decimal_u192_low_at_least(product, shift - 1u, 1) ? DECIMAL_ROUND_UP
                                                                 : DECIMAL_ROUND_TIE;
    }
    return decimal_u192_low_at_least(product, shift - 1u, error) ? DECIMAL_ROUND_UP
                                                                 : DECIMAL_ROUND_UNSURE;
}

static unsigned decimal_bit_length_64(uint64_t value)
{
    unsigned length = 0;

    while (value != 0)
    {
        length++;
        value >>= 1;
    }
    return length;
}

/* round(c * 2^q * 10^t), ties to even, where 64 by 128 bits decide it and
 * the result fits in 64 bits. */
static bool decimal_scaled_round(uint64_t c, int32_t q, int32_t t, uint64_t *rounded)
{
    const uint64_t *g = NULL;
    int32_t shift = 0;
    decimal_u192 product = {{0}};
    uint64_t result = 0;

    if (t < TB_DECIMAL_TABLE_LOWEST || t > TB_DECIMAL_TABLE_HIGHEST)
    {
        return false;
    }
    g = tb_decimal_table[t - TB_DECIMAL_TABLE_LOWEST];
    /* The entry is 10^t / 2^(floor(log2(10^t)) + 1 - 128). */
    shift = -(q + decimal_floor_log2_pow10(t) + 1 - 128);
    if (shift < 1)
    {
        return false;
    }
    product = decimal_multiply_entry(c, g);
    if (shift > DECIMAL_PRODUCT_BITS)
    {
        /* Less than half: the product has fewer bits than the shift. */
        *rounded = 0;
        return true;
    }
    if (decimal_u192_any_from(&product, (unsigned)shift + 64u))
    {
        return false;
    }
    result = decimal_u192_bits_from(&product, (unsigned)shift);
    switch (decimal_rounding_of(&product, (unsigned)shift,
                                t >= 0 && t <= DECIMAL_TABLE_EXACT_HIGHEST ? 0 : c))
    {
    case DECIMAL_ROUND_DOWN:
        break;
    case DECIMAL_ROUND_UP:
        result++;
        break;
    case DECIMAL_ROUND_TIE:
        result += result & 1u;
        break;
    case DECIMAL_ROUND_UNSURE:
        return false;
    }
    /* Rounding up past 64 bits wraps to zero, which no value that rounds
     * up can be. */
    if (result == 0 && decimal_u192_bit(&product, (unsigned)shift - 1u))
    {
        return false;
    }
    *rounded = result;
    return true;
}

/* The digits of `value` * 10^exponent, or zero. */
static void decimal_from_rounded(uint64_t value, int exponent, tb_decimal_exact *result)
{
    if (value == 0)
    {
        result->count = 0;
        result->exponent = 0;
        return;
    }
    decimal_from_integer(value, exponent, result);
}

bool tb_decimal_round_significant_fast(uint64_t mantissa, int exponent, size_t significant,
                                       tb_decimal_exact *result)
{
    uint64_t limit = 1;
    uint64_t rounded = 0;
    int32_t t = 0;

    TB_ASSERT(result != NULL);
    TB_ASSERT(significant >= 1);
    if (mantissa == 0)
    {
        decimal_from_rounded(0, 0, result);
        return true;
    }
    if (significant > DECIMAL_FAST_SIGNIFICANT_MAX)
    {
        return false;
    }
    for (size_t digit = 0; digit < significant; digit++)
    {
        limit *= 10u;
    }
    /* The value is in [2^b, 2^(b+1)), so its decimal exponent is this or
     * one more, and the first try scales it to [10^(s-1), 10^(s+1)). */
    t = (int32_t)significant - 1 -
        decimal_floor_log10_pow2(exponent + (int32_t)decimal_bit_length_64(mantissa) - 1, false);
    if (!decimal_scaled_round(mantissa, exponent, t, &rounded))
    {
        return false;
    }
    /* Past 10^s, the exponent was one more. Exactly 10^s is a value that
     * rounds up to it, at either exponent: 1 at the place above. */
    if (rounded > limit)
    {
        t--;
        if (!decimal_scaled_round(mantissa, exponent, t, &rounded))
        {
            return false;
        }
    }
    decimal_from_rounded(rounded, -t, result);
    return true;
}

bool tb_decimal_round_fraction_fast(uint64_t mantissa, int exponent, size_t fraction_digits,
                                    tb_decimal_exact *result)
{
    uint64_t rounded = 0;

    TB_ASSERT(result != NULL);
    if (fraction_digits > (size_t)TB_DECIMAL_TABLE_HIGHEST)
    {
        return false;
    }
    if (!decimal_scaled_round(mantissa, exponent, (int32_t)fraction_digits, &rounded))
    {
        return false;
    }
    decimal_from_rounded(rounded, -(int)fraction_digits, result);
    return true;
}

void tb_decimal_round_significant(uint64_t mantissa, int exponent, size_t significant,
                                  tb_decimal_exact *result)
{
    if (tb_decimal_round_significant_fast(mantissa, exponent, significant, result))
    {
        return;
    }
    tb_decimal_exact_of(mantissa, exponent, result);
    tb_decimal_round(result, (ptrdiff_t)significant);
}

void tb_decimal_round_fraction(uint64_t mantissa, int exponent, size_t fraction_digits,
                               tb_decimal_exact *result)
{
    if (tb_decimal_round_fraction_fast(mantissa, exponent, fraction_digits, result))
    {
        return;
    }
    tb_decimal_exact_of(mantissa, exponent, result);
    if (result->count > 0)
    {
        tb_decimal_round(result, (ptrdiff_t)result->exponent + 1 + (ptrdiff_t)fraction_digits);
    }
}

bool tb_decimal_read_fast(uint64_t digits, int64_t exponent, bool negative, double *value)
{
    const unsigned zeros = 64u - decimal_bit_length_64(digits);
    uint64_t normalized = 0;
    const uint64_t *g = NULL;
    int32_t binary_exponent = 0;
    decimal_u192 product = {{0}};
    uint64_t error = 0;
    unsigned top = 0;
    unsigned shift = 0;
    uint64_t mantissa = 0;

    TB_ASSERT(value != NULL);
    if (digits == 0 || exponent < TB_DECIMAL_TABLE_LOWEST || exponent > TB_DECIMAL_TABLE_HIGHEST)
    {
        return false;
    }
    normalized = digits << zeros;
    g = tb_decimal_table[exponent - TB_DECIMAL_TABLE_LOWEST];
    product = decimal_multiply_entry(normalized, g);
    error = exponent >= 0 && exponent <= DECIMAL_TABLE_EXACT_HIGHEST ? 0 : normalized;
    /* The true product, of at least 2^63 and 2^127, is at least 2^190. Its
     * top bit is 191 only if it is past 2^191 by more than the error. */
    top = decimal_u192_bit(&product, DECIMAL_PRODUCT_BITS - 1u) ? DECIMAL_PRODUCT_BITS - 1u
                                                                : DECIMAL_PRODUCT_BITS - 2u;
    if (top == DECIMAL_PRODUCT_BITS - 1u && error != 0 &&
        !decimal_u192_low_at_least(&product, top, error))
    {
        return false;
    }
    /* Keep the 53 bits from the top. */
    shift = top - DECIMAL_FRACTION_BITS;
    mantissa = decimal_u192_bits_from(&product, shift);
    switch (decimal_rounding_of(&product, shift, error))
    {
    case DECIMAL_ROUND_DOWN:
        break;
    case DECIMAL_ROUND_UP:
        mantissa++;
        break;
    case DECIMAL_ROUND_TIE:
        mantissa += mantissa & 1u;
        break;
    case DECIMAL_ROUND_UNSURE:
        return false;
    }
    /* The value is mantissa * 2^binary_exponent. */
    binary_exponent =
        (int32_t)shift + decimal_floor_log2_pow10((int32_t)exponent) + 1 - 128 - (int32_t)zeros;
    if (mantissa == DECIMAL_HIDDEN_BIT << 1)
    {
        mantissa = DECIMAL_HIDDEN_BIT;
        binary_exponent++;
    }
    /* Subnormal and infinite results are left to the exact form. */
    if (binary_exponent < DECIMAL_LOWEST_BIT_EXPONENT ||
        binary_exponent >
            DECIMAL_SPECIAL_EXPONENT - 1 - DECIMAL_EXPONENT_BIAS - DECIMAL_FRACTION_BITS)
    {
        return false;
    }
    *value = decimal_value_of(
        (negative ? DECIMAL_SIGN_BIT : 0) |
        ((uint64_t)(binary_exponent - DECIMAL_LOWEST_BIT_EXPONENT + 1) << DECIMAL_FRACTION_BITS) |
        (mantissa & DECIMAL_FRACTION_MASK));
    return true;
}

/* Appends bytes to a text being built in a buffer that is large enough. */
typedef struct decimal_text
{
    char bytes[TB_DECIMAL_SHORTEST_SIZE];
    size_t length;
} decimal_text;

static void decimal_append(decimal_text *text, const char *bytes, size_t length)
{
    TB_ASSERT(text->length + length < sizeof(text->bytes));
    memcpy(text->bytes + text->length, bytes, length);
    text->length += length;
}

static void decimal_append_repeated(decimal_text *text, char byte, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        decimal_append(text, &byte, 1);
    }
}

/* Python's repr of the digits: fixed notation for exponents from -4 to 15,
 * with at least one digit after the point, and otherwise one digit, the rest
 * after a point, and an exponent of at least two digits. */
static void decimal_write_repr(const tb_decimal_exact *exact, decimal_text *text)
{
    const int exponent = exact->exponent;

    if (exponent >= DECIMAL_FIXED_LOWEST_EXPONENT && exponent < DECIMAL_FIXED_LIMIT_EXPONENT)
    {
        if (exponent < 0)
        {
            decimal_append(text, "0.", 2);
            decimal_append_repeated(text, '0', (size_t)(-exponent - 1));
            decimal_append(text, exact->digits, exact->count);
            return;
        }
        {
            const size_t integer_digits = (size_t)exponent + 1;

            if (exact->count <= integer_digits)
            {
                decimal_append(text, exact->digits, exact->count);
                decimal_append_repeated(text, '0', integer_digits - exact->count);
                decimal_append(text, ".0", 2);
                return;
            }
            decimal_append(text, exact->digits, integer_digits);
            decimal_append(text, ".", 1);
            decimal_append(text, exact->digits + integer_digits, exact->count - integer_digits);
            return;
        }
    }
    {
        const unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
        char exponent_text[4] = {0};

        decimal_append(text, exact->digits, 1);
        if (exact->count > 1)
        {
            decimal_append(text, ".", 1);
            decimal_append(text, exact->digits + 1, exact->count - 1);
        }
        decimal_append(text, exponent < 0 ? "e-" : "e+", 2);
        exponent_text[0] = (char)('0' + magnitude / 100);
        exponent_text[1] = (char)('0' + magnitude / 10 % 10);
        exponent_text[2] = (char)('0' + magnitude % 10);
        if (magnitude >= 100)
        {
            decimal_append(text, exponent_text, 3);
        }
        else
        {
            decimal_append(text, exponent_text + 1, 2);
        }
    }
}

tb_decimal_result tb_decimal_shortest(double value, char *buffer, size_t size, size_t *required)
{
    const tb_decimal_parts parts = tb_decimal_split(value);
    decimal_text text = {0};

    TB_ASSERT(buffer != NULL || size == 0);
    TB_ASSERT(required != NULL);
    if (parts.is_nan)
    {
        decimal_append(&text, "nan", 3);
    }
    else
    {
        if (parts.negative)
        {
            decimal_append(&text, "-", 1);
        }
        if (parts.is_infinite)
        {
            decimal_append(&text, "inf", 3);
        }
        else if (parts.mantissa == 0)
        {
            decimal_append(&text, "0.0", 3);
        }
        else
        {
            tb_decimal_exact digits = {0};

            tb_decimal_shortest_fast(&parts, &digits);
            decimal_write_repr(&digits, &text);
        }
    }
    *required = text.length + 1;
    if (size < *required)
    {
        return TB_DECIMAL_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, text.bytes, text.length);
    buffer[text.length] = '\0';
    return TB_DECIMAL_OK;
}

/* --- reading: rounding a binary value --------------------------------------------------- */

static size_t decimal_bit_length(const tb_xp_digit *number, size_t length)
{
    const size_t significant = tb_xp_length(length, number);
    tb_xp_digit top = number[significant - 1];
    size_t bits = (significant - 1) * TB_XP_DIGIT_BITS;

    while (top != 0)
    {
        bits++;
        top >>= 1;
    }
    return bits;
}

static bool decimal_bit_is_set(const tb_xp_digit *number, size_t length, size_t bit)
{
    const size_t index = bit / TB_XP_DIGIT_BITS;

    if (index >= length)
    {
        return false;
    }
    return ((number[index] >> (bit % TB_XP_DIGIT_BITS)) & 1u) != 0;
}

/* Whether any bit below `bit` is set. */
static bool decimal_any_bit_below(const tb_xp_digit *number, size_t length, size_t bit)
{
    const size_t whole = bit / TB_XP_DIGIT_BITS;
    const unsigned rest = (unsigned)(bit % TB_XP_DIGIT_BITS);

    for (size_t index = 0; index < whole && index < length; index++)
    {
        if (number[index] != 0)
        {
            return true;
        }
    }
    if (whole < length && rest > 0)
    {
        return (number[whole] & (((tb_xp_digit)1 << rest) - 1)) != 0;
    }
    return false;
}

/* The double nearest to (number + a little more, when `sticky`) * 2^lowest,
 * ties to even, for a number that is not zero. The number may have any
 * length; its bits below the 53 a double keeps only decide the rounding. */
static double decimal_round_binary(const tb_xp_digit *number, size_t length, bool sticky,
                                   int64_t lowest, bool negative)
{
    const uint64_t sign = negative ? DECIMAL_SIGN_BIT : 0;
    const size_t bits = decimal_bit_length(number, length);
    const int64_t top = lowest + (int64_t)bits - 1;
    int64_t kept_lowest = top - DECIMAL_FRACTION_BITS;
    uint64_t mantissa = 0;

    if (top > DECIMAL_EXPONENT_BIAS)
    {
        return decimal_value_of(sign | DECIMAL_INFINITY_BITS);
    }
    if (kept_lowest < DECIMAL_LOWEST_BIT_EXPONENT)
    {
        kept_lowest = DECIMAL_LOWEST_BIT_EXPONENT;
    }
    if (kept_lowest <= lowest)
    {
        /* Every bit is kept, and fits: there are at most 53 of them, and
         * they move up by no more than the room left above them. */
        const int64_t shift = lowest - kept_lowest;
        uint64_t low_bits = 0;

        TB_ASSERT(shift >= 0 && shift <= DECIMAL_FRACTION_BITS);
        (void)tb_xp_to_u64(length, number, &low_bits);
        mantissa = low_bits << (unsigned)shift;
    }
    else
    {
        const size_t dropped = (size_t)(kept_lowest - lowest);
        const bool half = decimal_bit_is_set(number, length, dropped - 1);
        const bool more = sticky || decimal_any_bit_below(number, length, dropped - 1);
        tb_xp_digit shifted[DECIMAL_NUMERATOR_DIGITS] = {0};

        TB_ASSERT(length <= DECIMAL_NUMERATOR_DIGITS);
        tb_xp_shift_right(length, shifted, number, dropped);
        (void)tb_xp_to_u64(length, shifted, &mantissa);
        if (half && (more || (mantissa % 2) != 0))
        {
            mantissa++;
        }
    }
    if (mantissa > DECIMAL_FRACTION_MASK * 2 + 1)
    {
        /* Rounding carried into a 54th bit. */
        mantissa >>= 1;
        kept_lowest++;
    }
    if (mantissa >= DECIMAL_HIDDEN_BIT)
    {
        const int64_t biased = kept_lowest + DECIMAL_EXPONENT_BIAS + DECIMAL_FRACTION_BITS;

        if (biased >= DECIMAL_SPECIAL_EXPONENT)
        {
            return decimal_value_of(sign | DECIMAL_INFINITY_BITS);
        }
        return decimal_value_of(sign | ((uint64_t)biased << DECIMAL_FRACTION_BITS) |
                                (mantissa & DECIMAL_FRACTION_MASK));
    }
    /* Subnormal, or zero when even the lowest bit rounded away. */
    return decimal_value_of(sign | mantissa);
}

/* --- reading: the forms of text -------------------------------------------------------- */

static bool decimal_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

static unsigned decimal_hex_value(char character)
{
    if (decimal_is_digit(character))
    {
        return (unsigned)(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return (unsigned)(character - 'a') + 10u;
    }
    if (character >= 'A' && character <= 'F')
    {
        return (unsigned)(character - 'A') + 10u;
    }
    return 16u;
}

static char decimal_lower(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character - 'A' + 'a');
    }
    return character;
}

/* Whether the text is a word, in any case. */
static bool decimal_is_word(const char *text, size_t length, const char *word)
{
    size_t index = 0;

    for (index = 0; index < length; index++)
    {
        if (word[index] == '\0' || decimal_lower(text[index]) != word[index])
        {
            return false;
        }
    }
    return word[index] == '\0';
}

/* Reads an exponent: an optional sign and at least one digit, ending the
 * text. Saturates, since any exponent beyond the saturation has the same
 * effect. */
static bool decimal_read_exponent(const char *text, size_t length, int64_t *exponent)
{
    size_t position = 0;
    bool negative = false;
    int64_t magnitude = 0;

    if (position < length && (text[position] == '+' || text[position] == '-'))
    {
        negative = text[position] == '-';
        position++;
    }
    if (position == length)
    {
        return false;
    }
    for (; position < length; position++)
    {
        if (!decimal_is_digit(text[position]))
        {
            return false;
        }
        if (magnitude < DECIMAL_EXPONENT_SATURATION)
        {
            magnitude = magnitude * 10 + (text[position] - '0');
        }
    }
    *exponent = negative ? -magnitude : magnitude;
    return true;
}

/* The significant digits of a decimal number and the power of ten of the
 * last of them. */
typedef struct decimal_reading
{
    char digits[DECIMAL_READ_DIGITS + 1];
    size_t count;
    int64_t exponent; /* The value is digits * 10^exponent. */
    bool sticky;      /* A nonzero digit was dropped after the kept ones. */
} decimal_reading;

static void decimal_take_digit(decimal_reading *reading, char digit, bool is_fraction)
{
    if (reading->count == 0 && digit == '0')
    {
        /* A leading zero: in the fraction it moves the point. */
        if (is_fraction)
        {
            reading->exponent--;
        }
        return;
    }
    if (reading->count < DECIMAL_READ_DIGITS)
    {
        reading->digits[reading->count] = digit;
        reading->count++;
        if (is_fraction)
        {
            reading->exponent--;
        }
        return;
    }
    /* Beyond the kept digits: an integer digit still scales the value. */
    if (!is_fraction && reading->exponent < DECIMAL_EXPONENT_SATURATION)
    {
        reading->exponent++;
    }
    if (digit != '0')
    {
        reading->sticky = true;
    }
}

/* Multiplies by 10^count, or makes the number 10^count when it is 1. */
static void decimal_scale_by_ten(tb_xp_digit *number, size_t *length, size_t capacity, size_t count)
{
    decimal_multiply_power(number, length, capacity, 10, count, DECIMAL_TEN_TO_THE_9,
                           DECIMAL_TEN_STEP);
}

/* digits * 10^exponent, exactly, rounded to the nearest double. */
static double decimal_value_of_reading(decimal_reading *reading, bool negative)
{
    tb_xp_digit numerator[DECIMAL_NUMERATOR_DIGITS] = {0};
    tb_xp_digit denominator[DECIMAL_DENOMINATOR_DIGITS] = {0};
    tb_xp_digit quotient[DECIMAL_NUMERATOR_DIGITS] = {0};
    tb_xp_digit remainder[DECIMAL_DENOMINATOR_DIGITS] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(DECIMAL_NUMERATOR_DIGITS, DECIMAL_DENOMINATOR_DIGITS)] = {
        0};
    size_t numerator_length = 0;
    size_t denominator_length = 1;
    int64_t magnitude = 0;
    int64_t shift = 0;
    tb_xp_result parsed = TB_XP_OK;

    if (reading->sticky)
    {
        reading->digits[reading->count] = '1';
        reading->count++;
        reading->exponent--;
    }
    /* The decimal exponent of the place above the first digit. */
    magnitude = (int64_t)reading->count + reading->exponent;
    if (magnitude > DECIMAL_LARGEST_DECIMAL_EXPONENT)
    {
        return decimal_value_of((negative ? DECIMAL_SIGN_BIT : 0) | DECIMAL_INFINITY_BITS);
    }
    if (magnitude < DECIMAL_SMALLEST_DECIMAL_EXPONENT)
    {
        return decimal_value_of(negative ? DECIMAL_SIGN_BIT : 0);
    }

    parsed =
        tb_xp_from_string(DECIMAL_NUMERATOR_DIGITS, numerator, reading->digits, reading->count, 10);
    TB_ASSERT(parsed == TB_XP_OK);
    numerator_length = tb_xp_length(DECIMAL_NUMERATOR_DIGITS, numerator);
    denominator[0] = 1;
    if (reading->exponent >= 0)
    {
        decimal_scale_by_ten(numerator, &numerator_length, DECIMAL_NUMERATOR_DIGITS,
                             (size_t)reading->exponent);
    }
    else
    {
        decimal_scale_by_ten(denominator, &denominator_length, DECIMAL_DENOMINATOR_DIGITS,
                             (size_t)-reading->exponent);
    }
    /* Shift the numerator so that the quotient has a few more bits than a
     * double keeps; the remainder only says whether anything is left. */
    shift = DECIMAL_QUOTIENT_BITS + (int64_t)decimal_bit_length(denominator, denominator_length) -
            (int64_t)decimal_bit_length(numerator, numerator_length);
    if (shift < 0)
    {
        shift = 0;
    }
    tb_xp_shift_left(DECIMAL_NUMERATOR_DIGITS, numerator, numerator, (size_t)shift);
    numerator_length = tb_xp_length(DECIMAL_NUMERATOR_DIGITS, numerator);
    tb_xp_div(numerator_length, quotient, numerator, denominator_length, denominator, remainder,
              scratch);
    return decimal_round_binary(
        quotient, numerator_length,
        !(tb_xp_length(denominator_length, remainder) == 1 && remainder[0] == 0), -shift, negative);
}

/* The kept digits as a number, read fast where they fit in 64 bits and
 * nothing was dropped after them. */
static bool decimal_read_reading_fast(const decimal_reading *reading, bool negative, double *value)
{
    uint64_t digits = 0;

    if (reading->sticky || reading->count > DECIMAL_FAST_READ_DIGITS)
    {
        return false;
    }
    for (size_t index = 0; index < reading->count; index++)
    {
        digits = digits * 10u + (uint64_t)(reading->digits[index] - '0');
    }
    return tb_decimal_read_fast(digits, reading->exponent, negative, value);
}

double tb_decimal_read_exact(uint64_t digits, int64_t exponent, bool negative)
{
    decimal_reading reading = {0};
    char text[DECIMAL_FAST_READ_DIGITS + 2] = {0};
    size_t count = 0;

    if (digits == 0)
    {
        return decimal_value_of(negative ? DECIMAL_SIGN_BIT : 0);
    }
    while (digits != 0)
    {
        text[count] = (char)('0' + (int)(digits % 10u));
        count++;
        digits /= 10u;
    }
    for (size_t index = 0; index < count; index++)
    {
        reading.digits[index] = text[count - 1 - index];
    }
    reading.count = count;
    reading.exponent = exponent;
    return decimal_value_of_reading(&reading, negative);
}

static bool decimal_read_decimal(const char *text, size_t length, bool negative, double *value)
{
    decimal_reading reading = {0};
    size_t position = 0;
    size_t digit_count = 0;
    int64_t exponent = 0;

    for (; position < length && decimal_is_digit(text[position]); position++)
    {
        decimal_take_digit(&reading, text[position], false);
        digit_count++;
    }
    if (position < length && text[position] == '.')
    {
        for (position++; position < length && decimal_is_digit(text[position]); position++)
        {
            decimal_take_digit(&reading, text[position], true);
            digit_count++;
        }
    }
    if (digit_count == 0)
    {
        return false;
    }
    if (position < length && (text[position] == 'e' || text[position] == 'E'))
    {
        if (!decimal_read_exponent(text + position + 1, length - position - 1, &exponent))
        {
            return false;
        }
        position = length;
    }
    if (position != length)
    {
        return false;
    }
    if (reading.count == 0)
    {
        *value = decimal_value_of(negative ? DECIMAL_SIGN_BIT : 0);
        return true;
    }
    reading.exponent += exponent;
    if (decimal_read_reading_fast(&reading, negative, value))
    {
        return true;
    }
    *value = decimal_value_of_reading(&reading, negative);
    return true;
}

/* Hexadecimal: up to 15 significant digits are kept in 64 bits, and the rest
 * only move the binary point or say that something is left. */
static bool decimal_read_hexadecimal(const char *text, size_t length, bool negative, double *value)
{
    uint64_t mantissa = 0;
    size_t kept = 0;
    size_t digit_count = 0;
    int64_t binary_exponent = 0;
    int64_t exponent = 0;
    bool sticky = false;
    bool is_fraction = false;
    size_t position = 0;
    tb_xp_digit number[2] = {0};

    for (; position < length; position++)
    {
        const unsigned digit = decimal_hex_value(text[position]);

        if (text[position] == '.' && !is_fraction)
        {
            is_fraction = true;
            continue;
        }
        if (digit >= 16u)
        {
            break;
        }
        digit_count++;
        if (kept == 0 && digit == 0)
        {
            binary_exponent -= is_fraction ? 4 : 0;
            continue;
        }
        if (kept < DECIMAL_HEX_DIGITS)
        {
            mantissa = mantissa * 16u + digit;
            kept++;
            binary_exponent -= is_fraction ? 4 : 0;
            continue;
        }
        binary_exponent += is_fraction ? 0 : 4;
        sticky = sticky || digit != 0;
    }
    if (digit_count == 0)
    {
        return false;
    }
    if (position < length && (text[position] == 'p' || text[position] == 'P'))
    {
        if (!decimal_read_exponent(text + position + 1, length - position - 1, &exponent))
        {
            return false;
        }
        position = length;
    }
    if (position != length)
    {
        return false;
    }
    binary_exponent += exponent;
    if (mantissa == 0 || binary_exponent < DECIMAL_HEX_SMALLEST_EXPONENT)
    {
        *value = decimal_value_of(negative ? DECIMAL_SIGN_BIT : 0);
        return true;
    }
    if (binary_exponent > DECIMAL_HEX_LARGEST_EXPONENT)
    {
        *value = decimal_value_of((negative ? DECIMAL_SIGN_BIT : 0) | DECIMAL_INFINITY_BITS);
        return true;
    }
    (void)tb_xp_from_u64(2, number, mantissa);
    *value = decimal_round_binary(number, 2, sticky, binary_exponent, negative);
    return true;
}

tb_decimal_result tb_decimal_parse(const char *text, size_t length, double *value)
{
    size_t position = 0;
    bool negative = false;
    const char *rest = NULL;
    size_t rest_length = 0;
    double result = 0.0;
    bool is_read = false;

    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(value != NULL);
    if (length > 0 && (text[0] == '+' || text[0] == '-'))
    {
        negative = text[0] == '-';
        position = 1;
    }
    rest = text + position;
    rest_length = length - position;
    if (decimal_is_word(rest, rest_length, "inf") || decimal_is_word(rest, rest_length, "infinity"))
    {
        result = decimal_value_of((negative ? DECIMAL_SIGN_BIT : 0) | DECIMAL_INFINITY_BITS);
        is_read = true;
    }
    else if (decimal_is_word(rest, rest_length, "nan"))
    {
        result = decimal_value_of((negative ? DECIMAL_SIGN_BIT : 0) | DECIMAL_QUIET_NAN_BITS);
        is_read = true;
    }
    else if (rest_length >= 2 && rest[0] == '0' && decimal_lower(rest[1]) == 'x')
    {
        is_read = decimal_read_hexadecimal(rest + 2, rest_length - 2, negative, &result);
    }
    else
    {
        is_read = decimal_read_decimal(rest, rest_length, negative, &result);
    }
    if (!is_read)
    {
        return TB_DECIMAL_INVALID;
    }
    *value = result;
    return TB_DECIMAL_OK;
}
