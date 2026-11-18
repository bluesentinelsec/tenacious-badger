#include "badger/xp.h"

#include "badger/assert.h"

#include <stdint.h>
#include <string.h>

/* One digit more than the largest digit, as a 64-bit value: 2^32. */
#define XP_RADIX ((uint64_t)1 << TB_XP_DIGIT_BITS)
#define XP_DIGIT_MASK ((uint64_t)UINT32_MAX)
#define XP_DIGIT_BYTES sizeof(tb_xp_digit)

/* A text digit's value for a byte that is no digit in any base. */
#define XP_NOT_A_DIGIT 255u

/* An upper bound on the number of text digits that one bit needs in each
 * base, log_base(2), scaled by 2^16 and rounded up. Indexed by base - 2. */
#define XP_SCALE_BITS 16u
#define XP_SCALE ((size_t)1 << XP_SCALE_BITS)
static const size_t xp_text_digits_per_bit[TB_XP_BASE_MAX - TB_XP_BASE_MIN + 1] = {
    65537, 41349, 32769, 28225, 25353, 23345, 21846, 20675, 19729, 18945, 18281, 17711,
    17213, 16775, 16385, 16034, 15717, 15428, 15164, 14921, 14697, 14488, 14294, 14113,
    13943, 13783, 13633, 13491, 13356, 13229, 13108, 12992, 12882, 12777, 12677};

static const char xp_digit_characters[] = "0123456789abcdefghijklmnopqrstuvwxyz";

const char *tb_xp_result_name(tb_xp_result result)
{
    switch (result)
    {
    case TB_XP_OK:
        return "ok";
    case TB_XP_INVALID:
        return "invalid argument";
    case TB_XP_TOO_LARGE:
        return "too large";
    case TB_XP_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown result";
    }
}

/* --- checks of the arguments ----------------------------------------------------- */

/* Whether two arrays of digits share any memory. The addresses are compared as
 * integers, because C defines no order between pointers into different arrays. */
static bool xp_overlap(const tb_xp_digit *first, size_t first_length, const tb_xp_digit *second,
                       size_t second_length)
{
    const uintptr_t first_begin = (uintptr_t)first;
    const uintptr_t first_end = first_begin + first_length * XP_DIGIT_BYTES;
    const uintptr_t second_begin = (uintptr_t)second;
    const uintptr_t second_end = second_begin + second_length * XP_DIGIT_BYTES;

    return first_begin < second_end && second_begin < first_end;
}

/* Whether an output may be written while the input is read: the two are one
 * array, or apart. */
static bool xp_same_or_apart(const tb_xp_digit *output, const tb_xp_digit *input, size_t n)
{
    return output == input || !xp_overlap(output, n, input, n);
}

static bool xp_is_zero(size_t n, const tb_xp_digit *x)
{
    for (size_t index = 0; index < n; index++)
    {
        if (x[index] != 0)
        {
            return false;
        }
    }
    return true;
}

static bool xp_base_is_valid(unsigned base)
{
    return base >= TB_XP_BASE_MIN && base <= TB_XP_BASE_MAX;
}

/* --- addition and subtraction ------------------------------------------------------ */

unsigned tb_xp_add(size_t n, tb_xp_digit *z, const tb_xp_digit *x, const tb_xp_digit *y,
                   unsigned carry)
{
    uint64_t running = carry;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL && y != NULL);
    TB_ASSERT(carry <= 1);
    TB_ASSERT(xp_same_or_apart(z, x, n) && xp_same_or_apart(z, y, n));
    for (size_t index = 0; index < n; index++)
    {
        running += (uint64_t)x[index] + y[index];
        z[index] = (tb_xp_digit)running;
        running >>= TB_XP_DIGIT_BITS;
    }
    return (unsigned)running;
}

unsigned tb_xp_sub(size_t n, tb_xp_digit *z, const tb_xp_digit *x, const tb_xp_digit *y,
                   unsigned borrow)
{
    unsigned running = borrow;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL && y != NULL);
    TB_ASSERT(borrow <= 1);
    TB_ASSERT(xp_same_or_apart(z, x, n) && xp_same_or_apart(z, y, n));
    for (size_t index = 0; index < n; index++)
    {
        /* Computed in 64 bits, a negative difference wraps to a value whose
         * top bit is set. */
        const uint64_t difference = (uint64_t)x[index] - y[index] - running;

        z[index] = (tb_xp_digit)difference;
        running = (unsigned)(difference >> 63);
    }
    return running;
}

unsigned tb_xp_neg(size_t n, tb_xp_digit *z, const tb_xp_digit *x, unsigned carry)
{
    uint64_t running = carry;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(carry <= 1);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    for (size_t index = 0; index < n; index++)
    {
        running += (tb_xp_digit)~x[index];
        z[index] = (tb_xp_digit)running;
        running >>= TB_XP_DIGIT_BITS;
    }
    return (unsigned)running;
}

unsigned tb_xp_sum(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y)
{
    uint64_t running = y;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    for (size_t index = 0; index < n; index++)
    {
        running += x[index];
        z[index] = (tb_xp_digit)running;
        running >>= TB_XP_DIGIT_BITS;
    }
    return (unsigned)running;
}

unsigned tb_xp_diff(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y)
{
    tb_xp_digit running = y;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    for (size_t index = 0; index < n; index++)
    {
        const uint64_t difference = (uint64_t)x[index] - running;

        z[index] = (tb_xp_digit)difference;
        running = (tb_xp_digit)(difference >> 63);
    }
    return (unsigned)running;
}

/* --- multiplication ---------------------------------------------------------------- */

/* accumulator[0..m-1] += y * multiplier. Returns the digit carried beyond
 * them. (2^32 - 1)^2 + 2 * (2^32 - 1) is 2^64 - 1, so no step overflows. */
static tb_xp_digit xp_multiply_add(size_t m, tb_xp_digit *accumulator, const tb_xp_digit *y,
                                   uint64_t multiplier)
{
    uint64_t carry = 0;

    for (size_t index = 0; index < m; index++)
    {
        const uint64_t partial = multiplier * y[index] + accumulator[index] + carry;

        accumulator[index] = (tb_xp_digit)partial;
        carry = partial >> TB_XP_DIGIT_BITS;
    }
    return (tb_xp_digit)carry;
}

void tb_xp_mul(tb_xp_digit *z, size_t n, const tb_xp_digit *x, size_t m, const tb_xp_digit *y)
{
    TB_ASSERT(n > 0 && m > 0);
    TB_ASSERT(z != NULL && x != NULL && y != NULL);
    TB_ASSERT_MSG(!xp_overlap(z, n + m, x, n) && !xp_overlap(z, n + m, y, m),
                  "the product must not overlap an operand");
    memset(z, 0, (n + m) * XP_DIGIT_BYTES);
    /* Each row adds one digit of x times y into the digits of z from that
     * digit's place, and its carry into the digit above them, which no
     * earlier row has reached. */
    for (size_t row = 0; row < n; row++)
    {
        if (x[row] != 0)
        {
            z[row + m] = xp_multiply_add(m, z + row, y, x[row]);
        }
    }
}

tb_xp_digit tb_xp_product(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y)
{
    uint64_t carry = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    for (size_t index = 0; index < n; index++)
    {
        const uint64_t partial = (uint64_t)x[index] * y + carry;

        z[index] = (tb_xp_digit)partial;
        carry = partial >> TB_XP_DIGIT_BITS;
    }
    return (tb_xp_digit)carry;
}

/* --- division ------------------------------------------------------------------------ */

tb_xp_digit tb_xp_quotient(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y)
{
    uint64_t remainder = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT_MSG(y != 0, "division by zero");
    TB_ASSERT(xp_same_or_apart(z, x, n));
    for (size_t step = n; step > 0; step--)
    {
        const size_t index = step - 1;
        const uint64_t dividend = (remainder << TB_XP_DIGIT_BITS) | x[index];

        z[index] = (tb_xp_digit)(dividend / y);
        remainder = dividend % y;
    }
    return (tb_xp_digit)remainder;
}

static unsigned xp_leading_zeros(tb_xp_digit digit)
{
    unsigned count = 0;

    TB_ASSERT(digit != 0);
    while ((digit & ((tb_xp_digit)1 << (TB_XP_DIGIT_BITS - 1))) == 0)
    {
        digit <<= 1;
        count++;
    }
    return count;
}

/* The first estimate of a quotient digit from the two leading digits of the
 * remainder can be up to two too large. Two more digits, one of each, find
 * almost every such case before the costly multiply and subtract
 * (Knuth, TAOCP vol. 2, 4.3.1, step D3). */
static uint64_t xp_estimate_quotient_digit(const tb_xp_digit *remainder_top,
                                           const tb_xp_digit *divisor_top)
{
    const uint64_t leading = ((uint64_t)remainder_top[0] << TB_XP_DIGIT_BITS) | remainder_top[-1];
    uint64_t estimate = leading / divisor_top[0];
    uint64_t rest = leading % divisor_top[0];

    while (estimate >= XP_RADIX ||
           estimate * divisor_top[-1] > ((rest << TB_XP_DIGIT_BITS) | remainder_top[-2]))
    {
        estimate--;
        rest += divisor_top[0];
        if (rest >= XP_RADIX)
        {
            break;
        }
    }
    return estimate;
}

/* remainder[0..m] -= estimate * divisor[0..m-1]. Returns whether the result
 * went negative, which means the estimate was still one too large. */
static bool xp_multiply_and_subtract(tb_xp_digit *remainder, const tb_xp_digit *divisor, size_t m,
                                     uint64_t estimate)
{
    uint64_t carry = 0;
    uint64_t borrow = 0;
    uint64_t difference = 0;

    for (size_t index = 0; index < m; index++)
    {
        const uint64_t partial = estimate * divisor[index] + carry;

        carry = partial >> TB_XP_DIGIT_BITS;
        difference = (uint64_t)remainder[index] - (partial & XP_DIGIT_MASK) - borrow;
        remainder[index] = (tb_xp_digit)difference;
        borrow = difference >> 63;
    }
    difference = (uint64_t)remainder[m] - carry - borrow;
    remainder[m] = (tb_xp_digit)difference;
    return (difference >> 63) != 0;
}

/* remainder[0..m] += divisor[0..m-1], ignoring the carry out of the top,
 * which cancels the borrow of the subtraction it undoes. */
static void xp_add_back(tb_xp_digit *remainder, const tb_xp_digit *divisor, size_t m)
{
    const unsigned carry = tb_xp_add(m, remainder, remainder, divisor, 0);

    remainder[m] += (tb_xp_digit)carry;
}

/* Long division of x by y where y has at least two significant digits and x
 * at least as many: Knuth, TAOCP vol. 2, 4.3.1, Algorithm D. */
static void xp_divide_long(size_t x_length, const tb_xp_digit *x, size_t y_length,
                           const tb_xp_digit *y, tb_xp_digit *q, tb_xp_digit *r,
                           tb_xp_digit *scratch)
{
    tb_xp_digit *const remainder = scratch;              /* x_length + 1 digits */
    tb_xp_digit *const divisor = scratch + x_length + 1; /* y_length digits */
    const unsigned shift = xp_leading_zeros(y[y_length - 1]);

    /* Normalize: shift both so that the divisor's top bit is set, which
     * keeps every estimate within two of the true quotient digit. */
    memcpy(divisor, y, y_length * XP_DIGIT_BYTES);
    tb_xp_shift_left(y_length, divisor, divisor, shift);
    memcpy(remainder, x, x_length * XP_DIGIT_BYTES);
    remainder[x_length] = 0;
    tb_xp_shift_left(x_length + 1, remainder, remainder, shift);

    for (size_t step = x_length - y_length + 1; step > 0; step--)
    {
        const size_t position = step - 1;
        tb_xp_digit *const window = remainder + position;
        uint64_t estimate = xp_estimate_quotient_digit(window + y_length, divisor + y_length - 1);

        if (xp_multiply_and_subtract(window, divisor, y_length, estimate))
        {
            estimate--;
            xp_add_back(window, divisor, y_length);
        }
        q[position] = (tb_xp_digit)estimate;
    }
    /* What is left is less than the normalized divisor, so it lies in the
     * low y_length digits; undo the normalization. */
    tb_xp_shift_right(y_length, remainder, remainder, shift);
    memcpy(r, remainder, y_length * XP_DIGIT_BYTES);
}

void tb_xp_div(size_t n, tb_xp_digit *q, const tb_xp_digit *x, size_t m, const tb_xp_digit *y,
               tb_xp_digit *r, tb_xp_digit *scratch)
{
    const size_t scratch_length = TB_XP_DIV_SCRATCH(n, m);
    size_t x_length = 0;
    size_t y_length = 0;

    TB_ASSERT(n > 0 && m > 0);
    TB_ASSERT(q != NULL && x != NULL && y != NULL && r != NULL && scratch != NULL);
    TB_ASSERT_MSG(!xp_is_zero(m, y), "division by zero");
    TB_ASSERT_MSG(!xp_overlap(q, n, x, n) && !xp_overlap(q, n, y, m) && !xp_overlap(q, n, r, m) &&
                      !xp_overlap(q, n, scratch, scratch_length),
                  "the quotient must not overlap another argument");
    TB_ASSERT_MSG(!xp_overlap(r, m, x, n) && !xp_overlap(r, m, y, m) &&
                      !xp_overlap(r, m, scratch, scratch_length),
                  "the remainder must not overlap another argument");
    TB_ASSERT_MSG(!xp_overlap(scratch, scratch_length, x, n) &&
                      !xp_overlap(scratch, scratch_length, y, m),
                  "the scratch digits must not overlap another argument");

    x_length = tb_xp_length(n, x);
    y_length = tb_xp_length(m, y);
    memset(q, 0, n * XP_DIGIT_BYTES);
    memset(r, 0, m * XP_DIGIT_BYTES);
    if (y_length == 1)
    {
        r[0] = tb_xp_quotient(n, q, x, y[0]);
        return;
    }
    if (x_length < y_length || (x_length == y_length && tb_xp_compare(x_length, x, y) < 0))
    {
        /* The quotient is zero and the dividend is the remainder; it fits in
         * r because it is shorter than the divisor. */
        memcpy(r, x, x_length * XP_DIGIT_BYTES);
        return;
    }
    xp_divide_long(x_length, x, y_length, y, q, r, scratch);
}

/* --- comparing and shifting ------------------------------------------------------ */

int tb_xp_compare(size_t n, const tb_xp_digit *x, const tb_xp_digit *y)
{
    TB_ASSERT(n > 0);
    TB_ASSERT(x != NULL && y != NULL);
    for (size_t step = n; step > 0; step--)
    {
        const size_t index = step - 1;

        if (x[index] != y[index])
        {
            return x[index] < y[index] ? -1 : 1;
        }
    }
    return 0;
}

void tb_xp_shift_left(size_t n, tb_xp_digit *z, const tb_xp_digit *x, size_t bits)
{
    size_t digit_shift = 0;
    unsigned bit_shift = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    digit_shift = bits / TB_XP_DIGIT_BITS;
    bit_shift = (unsigned)(bits % TB_XP_DIGIT_BITS);
    if (digit_shift >= n)
    {
        memset(z, 0, n * XP_DIGIT_BYTES);
        return;
    }
    /* From the top down, so that z may be x: each digit is written after the
     * digits it reads from, which are at or below it. */
    for (size_t step = n; step > digit_shift; step--)
    {
        const size_t index = step - 1;
        const size_t source = index - digit_shift;
        tb_xp_digit digit = (tb_xp_digit)(x[source] << bit_shift);

        if (bit_shift > 0 && source > 0)
        {
            digit |= x[source - 1] >> (TB_XP_DIGIT_BITS - bit_shift);
        }
        z[index] = digit;
    }
    memset(z, 0, digit_shift * XP_DIGIT_BYTES);
}

void tb_xp_shift_right(size_t n, tb_xp_digit *z, const tb_xp_digit *x, size_t bits)
{
    size_t digit_shift = 0;
    unsigned bit_shift = 0;
    size_t kept = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL && x != NULL);
    TB_ASSERT(xp_same_or_apart(z, x, n));
    digit_shift = bits / TB_XP_DIGIT_BITS;
    bit_shift = (unsigned)(bits % TB_XP_DIGIT_BITS);
    if (digit_shift >= n)
    {
        memset(z, 0, n * XP_DIGIT_BYTES);
        return;
    }
    kept = n - digit_shift;
    /* From the bottom up, so that z may be x: each digit is written after the
     * digits it reads from, which are at or above it. */
    for (size_t index = 0; index < kept; index++)
    {
        const size_t source = index + digit_shift;
        tb_xp_digit digit = x[source] >> bit_shift;

        if (bit_shift > 0 && source + 1 < n)
        {
            digit |= (tb_xp_digit)(x[source + 1] << (TB_XP_DIGIT_BITS - bit_shift));
        }
        z[index] = digit;
    }
    memset(z + kept, 0, digit_shift * XP_DIGIT_BYTES);
}

size_t tb_xp_length(size_t n, const tb_xp_digit *x)
{
    size_t length = n;

    TB_ASSERT(n > 0);
    TB_ASSERT(x != NULL);
    while (length > 1 && x[length - 1] == 0)
    {
        length--;
    }
    return length;
}

/* --- native integers ----------------------------------------------------------------- */

bool tb_xp_from_u64(size_t n, tb_xp_digit *z, uint64_t value)
{
    uint64_t rest = value;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL);
    for (size_t index = 0; index < n; index++)
    {
        z[index] = (tb_xp_digit)rest;
        rest >>= TB_XP_DIGIT_BITS;
    }
    return rest == 0;
}

bool tb_xp_to_u64(size_t n, const tb_xp_digit *x, uint64_t *value)
{
    uint64_t low = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(x != NULL && value != NULL);
    low = x[0];
    if (n > 1)
    {
        low |= (uint64_t)x[1] << TB_XP_DIGIT_BITS;
    }
    *value = low;
    return n <= 2 || xp_is_zero(n - 2, x + 2);
}

/* --- text ---------------------------------------------------------------------------- */

/* The value of a text digit in any base, or XP_NOT_A_DIGIT. */
static unsigned xp_digit_value(char character)
{
    if (character >= '0' && character <= '9')
    {
        return (unsigned)(character - '0');
    }
    if (character >= 'a' && character <= 'z')
    {
        return (unsigned)(character - 'a') + 10u;
    }
    if (character >= 'A' && character <= 'Z')
    {
        return (unsigned)(character - 'A') + 10u;
    }
    return XP_NOT_A_DIGIT;
}

/* The largest power of the base that fits in one digit, and its exponent:
 * how many text digits one step of a conversion handles. */
typedef struct xp_chunk
{
    tb_xp_digit power;
    size_t text_digits;
} xp_chunk;

static xp_chunk xp_chunk_for(unsigned base)
{
    xp_chunk chunk = {.power = base, .text_digits = 1};

    while ((uint64_t)chunk.power * base <= UINT32_MAX)
    {
        chunk.power *= base;
        chunk.text_digits++;
    }
    return chunk;
}

tb_xp_result tb_xp_from_string(size_t n, tb_xp_digit *z, const char *text, size_t length,
                               unsigned base)
{
    xp_chunk chunk = {0};
    size_t position = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(z != NULL);
    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(xp_base_is_valid(base));
    chunk = xp_chunk_for(base);
    if (length == 0)
    {
        return TB_XP_INVALID;
    }
    for (size_t index = 0; index < length; index++)
    {
        if (xp_digit_value(text[index]) >= base)
        {
            return TB_XP_INVALID;
        }
    }

    memset(z, 0, n * XP_DIGIT_BYTES);
    while (position < length)
    {
        const size_t remaining = length - position;
        const size_t step = remaining < chunk.text_digits ? remaining : chunk.text_digits;
        tb_xp_digit scale = 1;
        tb_xp_digit value = 0;

        for (size_t index = 0; index < step; index++)
        {
            scale *= base;
            value = value * base + xp_digit_value(text[position + index]);
        }
        if (tb_xp_product(n, z, z, scale) != 0 || tb_xp_sum(n, z, z, value) != 0)
        {
            return TB_XP_TOO_LARGE;
        }
        position += step;
    }
    return TB_XP_OK;
}

/* 10^9, the largest power of ten in a digit: the step of decimal text. */
#define XP_DECIMAL_CHUNK 1000000000u

/* tb_xp_quotient() by 10^9. Decimal is by far the most common base of text,
 * and with the divisor a constant the compiler replaces each division by a
 * multiplication, which is several times faster. */
static tb_xp_digit xp_quotient_by_decimal_chunk(size_t n, tb_xp_digit *z)
{
    uint64_t remainder = 0;

    for (size_t step = n; step > 0; step--)
    {
        const size_t index = step - 1;
        const uint64_t dividend = (remainder << TB_XP_DIGIT_BITS) | z[index];

        z[index] = (tb_xp_digit)(dividend / XP_DECIMAL_CHUNK);
        remainder = dividend % XP_DECIMAL_CHUNK;
    }
    return (tb_xp_digit)remainder;
}

/* Writes the text digits of x, least significant first, into output, or only
 * counts them when output is NULL. Destroys the copy of x in scratch. */
static size_t xp_render_reversed(size_t x_length, const tb_xp_digit *x, unsigned base,
                                 tb_xp_digit *scratch, char *output)
{
    const xp_chunk chunk = xp_chunk_for(base);
    size_t length = x_length;
    size_t count = 0;

    memcpy(scratch, x, x_length * XP_DIGIT_BYTES);
    for (;;)
    {
        tb_xp_digit piece = base == 10 ? xp_quotient_by_decimal_chunk(length, scratch)
                                       : tb_xp_quotient(length, scratch, scratch, chunk.power);
        bool is_last = false;

        length = tb_xp_length(length, scratch);
        is_last = length == 1 && scratch[0] == 0;
        /* Every piece but the most significant one is written in full, with
         * its leading zeros; the last stops at its highest nonzero digit. */
        for (size_t index = 0; index < chunk.text_digits; index++)
        {
            if (is_last && piece == 0 && index > 0)
            {
                break;
            }
            if (output != NULL)
            {
                output[count] = xp_digit_characters[piece % base];
            }
            count++;
            piece /= base;
        }
        if (is_last)
        {
            return count;
        }
    }
}

static void xp_reverse(char *text, size_t length)
{
    size_t low = 0;
    size_t high = length;

    while (high - low > 1)
    {
        const char saved = text[low];

        high--;
        text[low] = text[high];
        text[high] = saved;
        low++;
    }
}

tb_xp_result tb_xp_to_string(size_t n, const tb_xp_digit *x, unsigned base, tb_xp_digit *scratch,
                             char *buffer, size_t size, size_t *required)
{
    size_t x_length = 0;
    size_t count = 0;

    TB_ASSERT(n > 0);
    TB_ASSERT(x != NULL && scratch != NULL && required != NULL);
    TB_ASSERT(buffer != NULL || size == 0);
    TB_ASSERT(xp_base_is_valid(base));
    TB_ASSERT_MSG(!xp_overlap(scratch, n, x, n), "the scratch digits must not overlap the number");

    x_length = tb_xp_length(n, x);
    if (size < tb_xp_string_size(x_length, base))
    {
        /* The buffer may be too small: measure before writing anything. */
        count = xp_render_reversed(x_length, x, base, scratch, NULL);
        *required = count + 1;
        if (size < *required)
        {
            return TB_XP_BUFFER_TOO_SMALL;
        }
    }
    /* The text fits, so the size is not 0, and the contract requires a
     * buffer. */
    TB_ASSERT(buffer != NULL);
    count = xp_render_reversed(x_length, x, base, scratch, buffer);
    xp_reverse(buffer, count);
    buffer[count] = '\0';
    *required = count + 1;
    return TB_XP_OK;
}

size_t tb_xp_string_size(size_t n, unsigned base)
{
    size_t bits = 0;
    size_t per_bit = 0;
    size_t digits = 0;

    TB_ASSERT(n > 0 && n <= SIZE_MAX / (2 * TB_XP_DIGIT_BITS));
    TB_ASSERT(xp_base_is_valid(base));
    bits = n * TB_XP_DIGIT_BITS;
    per_bit = xp_text_digits_per_bit[base - TB_XP_BASE_MIN];
    /* bits * per_bit / 2^16, rounded up, in two parts so that nothing
     * overflows. per_bit / 2^16 is at most a little over 1, and bits is at
     * most half of SIZE_MAX. */
    digits = (bits >> XP_SCALE_BITS) * per_bit +
             (((bits & (XP_SCALE - 1)) * per_bit + XP_SCALE - 1) >> XP_SCALE_BITS);
    return digits + 1;
}
