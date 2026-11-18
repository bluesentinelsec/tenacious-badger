#include "badger/ap.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "badger/xp.h"

#include <string.h>

/* A value is one block: this header, then `capacity` digits. The digits are
 * the magnitude, least significant first, of which the low `length` are
 * significant. Zero has a length of 1 and a positive sign, so that every
 * value has one representation. */
struct tb_ap
{
    tb_allocator allocator; /* Where the block came from. */
    size_t capacity;        /* Digits in the block. */
    size_t length;          /* Significant digits, from 1 to capacity. */
    int sign;               /* 1 or -1. */
};

#define AP_DIGIT_SIZE sizeof(tb_xp_digit)
#define AP_ALIGNMENT _Alignof(tb_ap)

/* The header is counted in digits, so that a block is an array of digits. */
TB_STATIC_ASSERT(sizeof(tb_ap) % sizeof(tb_xp_digit) == 0,
                 "the header is a whole number of digits");
#define AP_HEADER_DIGITS (sizeof(tb_ap) / AP_DIGIT_SIZE)

/* The formatter writes padding this many bytes at a time. */
#define AP_PAD_CHUNK 16

/* A magnitude and a sign that the operations read: a value, or a native
 * integer in two digits on the caller's stack. */
typedef struct ap_view
{
    const tb_xp_digit *digits;
    size_t length;
    int sign;
} ap_view;

const char *tb_ap_result_name(tb_ap_result result)
{
    switch (result)
    {
    case TB_AP_OK:
        return "ok";
    case TB_AP_INVALID:
        return "invalid argument";
    case TB_AP_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown result";
    }
}

/* --- values ------------------------------------------------------------------------ */

static tb_xp_digit *ap_digits(tb_ap *x)
{
    return (tb_xp_digit *)(void *)(x + 1);
}

static const tb_xp_digit *ap_const_digits(const tb_ap *x)
{
    return (const tb_xp_digit *)(const void *)(x + 1);
}

/* Digits in the block of a value of `capacity` digits. A count that cannot be
 * represented is a block no allocator can supply: SIZE_MAX makes the
 * allocation fail through the memory failure handler like any exhaustion. */
static size_t ap_block_digits(size_t capacity)
{
    size_t total = 0;

    if (!tb_size_add(capacity, AP_HEADER_DIGITS, &total))
    {
        return SIZE_MAX;
    }
    return total;
}

/* A new value of zero with room for `capacity` digits. */
static tb_ap *ap_new(const tb_allocator *allocator, size_t capacity)
{
    tb_ap *value = NULL;

    TB_ASSERT(capacity > 0);
    value = tb_allocator_calloc(allocator, ap_block_digits(capacity), AP_DIGIT_SIZE, AP_ALIGNMENT);
    value->allocator = tb_allocator_resolve(allocator);
    value->capacity = capacity;
    value->length = 1;
    value->sign = 1;
    return value;
}

/* Sets the length from the digits, and makes zero positive. */
static tb_ap *ap_normalize(tb_ap *x, int sign)
{
    x->length = tb_xp_length(x->capacity, ap_digits(x));
    x->sign = sign;
    if (x->length == 1 && ap_digits(x)[0] == 0)
    {
        x->sign = 1;
    }
    return x;
}

static ap_view ap_view_of(const tb_ap *x)
{
    const ap_view view = {.digits = ap_const_digits(x), .length = x->length, .sign = x->sign};

    return view;
}

static uint64_t ap_magnitude_of_i64(int64_t value)
{
    /* Computed in unsigned arithmetic, where the magnitude of INT64_MIN fits. */
    if (value < 0)
    {
        return (uint64_t)0 - (uint64_t)value;
    }
    return (uint64_t)value;
}

static ap_view ap_view_of_i64(int64_t value, tb_xp_digit storage[2])
{
    ap_view view = {.digits = storage, .length = 0, .sign = value < 0 ? -1 : 1};

    (void)tb_xp_from_u64(2, storage, ap_magnitude_of_i64(value));
    view.length = tb_xp_length(2, storage);
    return view;
}

static bool ap_view_is_zero(ap_view view)
{
    return view.length == 1 && view.digits[0] == 0;
}

static tb_ap *ap_from_view(const tb_allocator *allocator, ap_view view)
{
    tb_ap *const value = ap_new(allocator, view.length);

    memcpy(ap_digits(value), view.digits, view.length * AP_DIGIT_SIZE);
    return ap_normalize(value, view.sign);
}

tb_ap *tb_ap_from_i64(const tb_allocator *allocator, int64_t value)
{
    tb_xp_digit storage[2] = {0};

    return ap_from_view(allocator, ap_view_of_i64(value, storage));
}

tb_ap *tb_ap_from_u64(const tb_allocator *allocator, uint64_t value)
{
    tb_ap *const result = ap_new(allocator, 2);

    (void)tb_xp_from_u64(2, ap_digits(result), value);
    return ap_normalize(result, 1);
}

tb_ap *tb_ap_copy(const tb_ap *x, const tb_allocator *allocator)
{
    TB_ASSERT(x != NULL);
    return ap_from_view(allocator, ap_view_of(x));
}

void tb_ap_free(tb_ap *x)
{
    tb_allocator allocator = {0};

    if (x == NULL)
    {
        return;
    }
    allocator = x->allocator;
    tb_allocator_free_array(&allocator, x, ap_block_digits(x->capacity), AP_DIGIT_SIZE,
                            AP_ALIGNMENT);
}

/* Working space of `count` digits from an allocator, for one operation. */
static tb_xp_digit *ap_scratch(const tb_allocator *allocator, size_t count)
{
    return tb_allocator_calloc(allocator, count, AP_DIGIT_SIZE, _Alignof(tb_xp_digit));
}

static void ap_scratch_free(const tb_allocator *allocator, tb_xp_digit *scratch, size_t count)
{
    tb_allocator_free_array(allocator, scratch, count, AP_DIGIT_SIZE, _Alignof(tb_xp_digit));
}

/* --- comparing ------------------------------------------------------------------- */

static int ap_compare_magnitudes(ap_view left, ap_view right)
{
    if (left.length != right.length)
    {
        return left.length < right.length ? -1 : 1;
    }
    return tb_xp_compare(left.length, left.digits, right.digits);
}

static int ap_compare_views(ap_view left, ap_view right)
{
    if (left.sign != right.sign)
    {
        return left.sign < right.sign ? -1 : 1;
    }
    return left.sign * ap_compare_magnitudes(left, right);
}

int tb_ap_compare(const tb_ap *x, const tb_ap *y)
{
    TB_ASSERT(x != NULL && y != NULL);
    return ap_compare_views(ap_view_of(x), ap_view_of(y));
}

int tb_ap_compare_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};

    TB_ASSERT(x != NULL);
    return ap_compare_views(ap_view_of(x), ap_view_of_i64(y, storage));
}

int tb_ap_sign(const tb_ap *x)
{
    TB_ASSERT(x != NULL);
    if (ap_view_is_zero(ap_view_of(x)))
    {
        return 0;
    }
    return x->sign;
}

/* --- addition and subtraction ------------------------------------------------- */

/* |left| + |right|, with the given sign. */
static tb_ap *ap_add_magnitudes(const tb_allocator *allocator, ap_view left, ap_view right,
                                int sign)
{
    const ap_view longer = left.length >= right.length ? left : right;
    const ap_view shorter = left.length >= right.length ? right : left;
    tb_ap *const result = ap_new(allocator, longer.length + 1);
    tb_xp_digit *const digits = ap_digits(result);
    unsigned carry = 0;

    memcpy(digits, longer.digits, longer.length * AP_DIGIT_SIZE);
    carry = tb_xp_add(shorter.length, digits, digits, shorter.digits, 0);
    /* The carry ripples up into the extra digit at the top, which holds it. */
    carry = tb_xp_sum(result->capacity - shorter.length, digits + shorter.length,
                      digits + shorter.length, carry);
    TB_ASSERT(carry == 0);
    return ap_normalize(result, sign);
}

/* |larger| - |smaller|, where |larger| >= |smaller|, with the given sign. */
static tb_ap *ap_subtract_magnitudes(const tb_allocator *allocator, ap_view larger, ap_view smaller,
                                     int sign)
{
    tb_ap *const result = ap_new(allocator, larger.length);
    tb_xp_digit *const digits = ap_digits(result);
    unsigned borrow = 0;

    memcpy(digits, larger.digits, larger.length * AP_DIGIT_SIZE);
    borrow = tb_xp_sub(smaller.length, digits, digits, smaller.digits, 0);
    if (larger.length > smaller.length)
    {
        borrow = tb_xp_diff(larger.length - smaller.length, digits + smaller.length,
                            digits + smaller.length, borrow);
    }
    TB_ASSERT(borrow == 0);
    return ap_normalize(result, sign);
}

static tb_ap *ap_add_views(const tb_allocator *allocator, ap_view left, ap_view right)
{
    if (left.sign == right.sign)
    {
        return ap_add_magnitudes(allocator, left, right, left.sign);
    }
    if (ap_compare_magnitudes(left, right) >= 0)
    {
        return ap_subtract_magnitudes(allocator, left, right, left.sign);
    }
    return ap_subtract_magnitudes(allocator, right, left, right.sign);
}

static tb_ap *ap_subtract_views(const tb_allocator *allocator, ap_view left, ap_view right)
{
    ap_view negated = right;

    negated.sign = -right.sign;
    return ap_add_views(allocator, left, negated);
}

tb_ap *tb_ap_neg(const tb_ap *x)
{
    ap_view view = {0};

    TB_ASSERT(x != NULL);
    view = ap_view_of(x);
    view.sign = -view.sign;
    return ap_from_view(&x->allocator, view);
}

tb_ap *tb_ap_add(const tb_ap *x, const tb_ap *y)
{
    TB_ASSERT(x != NULL && y != NULL);
    return ap_add_views(&x->allocator, ap_view_of(x), ap_view_of(y));
}

tb_ap *tb_ap_sub(const tb_ap *x, const tb_ap *y)
{
    TB_ASSERT(x != NULL && y != NULL);
    return ap_subtract_views(&x->allocator, ap_view_of(x), ap_view_of(y));
}

tb_ap *tb_ap_add_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};

    TB_ASSERT(x != NULL);
    return ap_add_views(&x->allocator, ap_view_of(x), ap_view_of_i64(y, storage));
}

tb_ap *tb_ap_sub_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};

    TB_ASSERT(x != NULL);
    return ap_subtract_views(&x->allocator, ap_view_of(x), ap_view_of_i64(y, storage));
}

/* --- multiplication ------------------------------------------------------------- */

static tb_ap *ap_multiply_views(const tb_allocator *allocator, ap_view left, ap_view right)
{
    tb_ap *const result = ap_new(allocator, left.length + right.length);

    tb_xp_mul(ap_digits(result), left.length, left.digits, right.length, right.digits);
    return ap_normalize(result, left.sign * right.sign);
}

tb_ap *tb_ap_mul(const tb_ap *x, const tb_ap *y)
{
    TB_ASSERT(x != NULL && y != NULL);
    return ap_multiply_views(&x->allocator, ap_view_of(x), ap_view_of(y));
}

tb_ap *tb_ap_mul_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};

    TB_ASSERT(x != NULL);
    return ap_multiply_views(&x->allocator, ap_view_of(x), ap_view_of_i64(y, storage));
}

/* --- division -------------------------------------------------------------------- */

/* Floor division: the quotient rounded toward negative infinity and the
 * remainder with the sign of the divisor. Either output may be NULL when it
 * is not wanted. */
static void ap_divide_views(const tb_allocator *allocator, ap_view dividend, ap_view divisor,
                            tb_ap **quotient, tb_ap **remainder)
{
    const size_t scratch_count = TB_XP_DIV_SCRATCH(dividend.length, divisor.length);
    tb_ap *q = NULL;
    tb_ap *r = NULL;
    tb_xp_digit *scratch = NULL;
    tb_xp_digit *q_digits = NULL;
    tb_xp_digit *r_digits = NULL;

    TB_ASSERT_MSG(!ap_view_is_zero(divisor), "division by zero");
    q = ap_new(allocator, dividend.length + 1);
    r = ap_new(allocator, divisor.length);
    scratch = ap_scratch(allocator, scratch_count);
    q_digits = ap_digits(q);
    r_digits = ap_digits(r);
    tb_xp_div(dividend.length, q_digits, dividend.digits, divisor.length, divisor.digits, r_digits,
              scratch);
    ap_scratch_free(allocator, scratch, scratch_count);
    (void)ap_normalize(r, 1);

    /* Truncation has rounded toward zero. When the exact quotient is
     * negative and not whole, floor is one further down, and the remainder
     * moves by the divisor to keep x = q * y + r. */
    if (dividend.sign != divisor.sign && !(r->length == 1 && r_digits[0] == 0))
    {
        const unsigned carry = tb_xp_sum(q->capacity, q_digits, q_digits, 1);

        TB_ASSERT(carry == 0);
        (void)tb_xp_sub(divisor.length, r_digits, divisor.digits, r_digits, 0);
    }
    (void)ap_normalize(q, dividend.sign * divisor.sign);
    (void)ap_normalize(r, divisor.sign);

    if (quotient != NULL)
    {
        *quotient = q;
    }
    else
    {
        tb_ap_free(q);
    }
    if (remainder != NULL)
    {
        *remainder = r;
    }
    else
    {
        tb_ap_free(r);
    }
}

tb_ap *tb_ap_div_floor(const tb_ap *x, const tb_ap *y)
{
    tb_ap *quotient = NULL;

    TB_ASSERT(x != NULL && y != NULL);
    ap_divide_views(&x->allocator, ap_view_of(x), ap_view_of(y), &quotient, NULL);
    return quotient;
}

tb_ap *tb_ap_mod_floor(const tb_ap *x, const tb_ap *y)
{
    tb_ap *remainder = NULL;

    TB_ASSERT(x != NULL && y != NULL);
    ap_divide_views(&x->allocator, ap_view_of(x), ap_view_of(y), NULL, &remainder);
    return remainder;
}

tb_ap *tb_ap_div_floor_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};
    tb_ap *quotient = NULL;

    TB_ASSERT(x != NULL);
    ap_divide_views(&x->allocator, ap_view_of(x), ap_view_of_i64(y, storage), &quotient, NULL);
    return quotient;
}

int64_t tb_ap_mod_floor_i64(const tb_ap *x, int64_t y)
{
    tb_xp_digit storage[2] = {0};
    tb_ap *remainder = NULL;
    int64_t value = 0;
    bool fits = false;

    TB_ASSERT(x != NULL);
    ap_divide_views(&x->allocator, ap_view_of(x), ap_view_of_i64(y, storage), NULL, &remainder);
    fits = tb_ap_to_i64(remainder, &value);
    TB_ASSERT(fits);
    tb_ap_free(remainder);
    return value;
}

/* --- powers ----------------------------------------------------------------------- */

/* Replaces *value with *value mod p, when there is a modulus. */
static void ap_reduce(tb_ap **value, const tb_ap *modulus)
{
    tb_ap *reduced = NULL;

    if (modulus == NULL)
    {
        return;
    }
    reduced = tb_ap_mod_floor(*value, modulus);
    tb_ap_free(*value);
    *value = reduced;
}

/* Replaces *value with (*value * factor) mod p. */
static void ap_multiply_into(tb_ap **value, const tb_ap *factor, const tb_ap *modulus)
{
    tb_ap *const product = tb_ap_mul(*value, factor);

    tb_ap_free(*value);
    *value = product;
    ap_reduce(value, modulus);
}

/* The number of significant bits of a magnitude; 0 for zero. */
static size_t ap_bit_length(ap_view view)
{
    tb_xp_digit top = view.digits[view.length - 1];
    size_t bits = (view.length - 1) * TB_XP_DIGIT_BITS;

    while (top != 0)
    {
        bits++;
        top >>= 1;
    }
    return bits;
}

static bool ap_bit_is_set(ap_view view, size_t bit)
{
    const tb_xp_digit digit = view.digits[bit / TB_XP_DIGIT_BITS];

    return ((digit >> (bit % TB_XP_DIGIT_BITS)) & 1u) != 0;
}

tb_ap *tb_ap_pow(const tb_ap *x, const tb_ap *y, const tb_ap *p)
{
    ap_view exponent = {0};
    size_t bit_count = 0;
    tb_ap *base = NULL;
    tb_ap *result = NULL;

    TB_ASSERT(x != NULL && y != NULL);
    TB_ASSERT_MSG(y->sign > 0, "the exponent must not be negative");
    TB_ASSERT_MSG(p == NULL || tb_ap_sign(p) > 0, "the modulus must be positive");

    exponent = ap_view_of(y);
    bit_count = ap_bit_length(exponent);
    base = tb_ap_copy(x, &x->allocator);
    result = tb_ap_from_i64(&x->allocator, 1);
    ap_reduce(&base, p);
    ap_reduce(&result, p);
    /* From the lowest bit of the exponent up: base holds x^(2^bit). */
    for (size_t bit = 0; bit < bit_count; bit++)
    {
        if (ap_bit_is_set(exponent, bit))
        {
            ap_multiply_into(&result, base, p);
        }
        if (bit + 1 < bit_count)
        {
            ap_multiply_into(&base, base, p);
        }
    }
    tb_ap_free(base);
    return result;
}

/* --- shifts ------------------------------------------------------------------------ */

tb_ap *tb_ap_shift_left(const tb_ap *x, size_t bits)
{
    size_t capacity = 0;
    tb_ap *result = NULL;

    TB_ASSERT(x != NULL);
    if (ap_view_is_zero(ap_view_of(x)))
    {
        return tb_ap_copy(x, &x->allocator);
    }
    /* A count that needs more digits than can be counted needs more memory
     * than there is; SIZE_MAX digits fail to allocate. */
    if (!tb_size_add(x->length + 1, bits / TB_XP_DIGIT_BITS, &capacity))
    {
        capacity = SIZE_MAX;
    }
    result = ap_new(&x->allocator, capacity);
    memcpy(ap_digits(result), ap_const_digits(x), x->length * AP_DIGIT_SIZE);
    tb_xp_shift_left(capacity, ap_digits(result), ap_digits(result), bits);
    return ap_normalize(result, x->sign);
}

/* Whether any of the lowest `bits` bits of a magnitude is set. */
static bool ap_low_bits_are_set(ap_view view, size_t bits)
{
    const size_t whole_digits = bits / TB_XP_DIGIT_BITS;
    const unsigned rest = (unsigned)(bits % TB_XP_DIGIT_BITS);

    for (size_t index = 0; index < whole_digits && index < view.length; index++)
    {
        if (view.digits[index] != 0)
        {
            return true;
        }
    }
    if (whole_digits < view.length && rest > 0)
    {
        const tb_xp_digit mask = ((tb_xp_digit)1 << rest) - 1;

        return (view.digits[whole_digits] & mask) != 0;
    }
    return false;
}

tb_ap *tb_ap_shift_right(const tb_ap *x, size_t bits)
{
    ap_view view = {0};
    tb_ap *result = NULL;
    tb_xp_digit *digits = NULL;

    TB_ASSERT(x != NULL);
    view = ap_view_of(x);
    result = ap_new(&x->allocator, view.length);
    digits = ap_digits(result);
    memcpy(digits, view.digits, view.length * AP_DIGIT_SIZE);
    tb_xp_shift_right(view.length, digits, digits, bits);
    /* The magnitude was rounded down, which for a negative value is toward
     * zero; floor is one further from zero when a bit was shifted out. */
    if (view.sign < 0 && ap_low_bits_are_set(view, bits))
    {
        const unsigned carry = tb_xp_sum(view.length, digits, digits, 1);

        TB_ASSERT(carry == 0);
    }
    return ap_normalize(result, view.sign);
}

/* --- native integers --------------------------------------------------------------- */

bool tb_ap_to_i64(const tb_ap *x, int64_t *value)
{
    uint64_t magnitude = 0;
    const uint64_t most_negative = (uint64_t)1 << 63;

    TB_ASSERT(x != NULL && value != NULL);
    if (!tb_xp_to_u64(x->length, ap_const_digits(x), &magnitude))
    {
        return false;
    }
    if (x->sign > 0)
    {
        if (magnitude > (uint64_t)INT64_MAX)
        {
            return false;
        }
        *value = (int64_t)magnitude;
        return true;
    }
    if (magnitude > most_negative)
    {
        return false;
    }
    /* -magnitude, computed without overflow: INT64_MIN when it is 2^63. */
    *value = magnitude == most_negative ? INT64_MIN : -(int64_t)magnitude;
    return true;
}

bool tb_ap_to_u64(const tb_ap *x, uint64_t *value)
{
    uint64_t magnitude = 0;

    TB_ASSERT(x != NULL && value != NULL);
    if (x->sign < 0 || !tb_xp_to_u64(x->length, ap_const_digits(x), &magnitude))
    {
        return false;
    }
    *value = magnitude;
    return true;
}

/* --- text ---------------------------------------------------------------------------- */

static bool ap_base_is_valid(unsigned base)
{
    return base >= TB_XP_BASE_MIN && base <= TB_XP_BASE_MAX;
}

/* The bits that one text digit of a base can add: ceil(log2(base)). */
static size_t ap_bits_per_text_digit(unsigned base)
{
    size_t bits = 0;

    while (((unsigned)1 << bits) < base)
    {
        bits++;
    }
    return bits;
}

tb_ap_result tb_ap_from_string(const tb_allocator *allocator, const char *text, size_t length,
                               unsigned base, tb_ap **value)
{
    size_t position = 0;
    int sign = 1;
    size_t digit_count = 0;
    size_t bits = 0;
    size_t capacity = SIZE_MAX;
    tb_ap *result = NULL;
    tb_xp_result parsed = TB_XP_OK;

    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(ap_base_is_valid(base));
    TB_ASSERT(value != NULL);
    if (length > 0 && (text[0] == '+' || text[0] == '-'))
    {
        sign = text[0] == '-' ? -1 : 1;
        position = 1;
    }
    digit_count = length - position;
    if (digit_count == 0)
    {
        return TB_AP_INVALID;
    }
    /* Text too long for its size in bits to be counted needs more memory than
     * there is; SIZE_MAX digits fail to allocate. */
    if (tb_size_multiply(digit_count, ap_bits_per_text_digit(base), &bits))
    {
        capacity = bits / TB_XP_DIGIT_BITS + 1;
    }
    result = ap_new(allocator, capacity);
    parsed = tb_xp_from_string(capacity, ap_digits(result), text + position, digit_count, base);
    if (parsed != TB_XP_OK)
    {
        /* The capacity holds any digits of this length, so only a byte that is
         * not a digit can fail. */
        TB_ASSERT(parsed == TB_XP_INVALID);
        tb_ap_free(result);
        return TB_AP_INVALID;
    }
    *value = ap_normalize(result, sign);
    return TB_AP_OK;
}

size_t tb_ap_string_size(const tb_ap *x, unsigned base)
{
    TB_ASSERT(x != NULL);
    TB_ASSERT(ap_base_is_valid(base));
    return tb_xp_string_size(x->length, base) + (x->sign < 0 ? 1u : 0u);
}

tb_ap_result tb_ap_to_string(const tb_ap *x, unsigned base, char *buffer, size_t size,
                             size_t *required)
{
    const size_t sign_size = (x != NULL && x->sign < 0) ? 1u : 0u;
    tb_xp_digit *scratch = NULL;
    size_t digits_required = 0;
    tb_xp_result written = TB_XP_OK;

    TB_ASSERT(x != NULL && required != NULL);
    TB_ASSERT(buffer != NULL || size == 0);
    TB_ASSERT(ap_base_is_valid(base));
    scratch = ap_scratch(&x->allocator, x->length);
    if (size <= sign_size)
    {
        /* No room for a digit: measure only. */
        written = tb_xp_to_string(x->length, ap_const_digits(x), base, scratch, NULL, 0,
                                  &digits_required);
    }
    else
    {
        written = tb_xp_to_string(x->length, ap_const_digits(x), base, scratch, buffer + sign_size,
                                  size - sign_size, &digits_required);
    }
    ap_scratch_free(&x->allocator, scratch, x->length);
    *required = digits_required + sign_size;
    if (written != TB_XP_OK)
    {
        return TB_AP_BUFFER_TOO_SMALL;
    }
    /* The text fit, so the size is not 0, and the contract requires a
     * buffer. */
    TB_ASSERT(buffer != NULL);
    if (sign_size > 0)
    {
        buffer[0] = '-';
    }
    return TB_AP_OK;
}

/* --- formatting ---------------------------------------------------------------------- */

static void ap_put_repeated(tb_fmt_output *output, char character, size_t count)
{
    char chunk[AP_PAD_CHUNK] = {0};
    size_t left = count;

    memset(chunk, character, sizeof(chunk));
    while (left > 0)
    {
        const size_t step = left < sizeof(chunk) ? left : sizeof(chunk);

        tb_fmt_put(output, chunk, step);
        left -= step;
    }
}

/* The sign that a conversion writes before a value: '-', '+', ' ', or none. */
static char ap_sign_character(const tb_ap *x, const tb_fmt_spec *spec)
{
    if (x->sign < 0)
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

void tb_ap_format(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
{
    const tb_ap *const x = va_arg(*arguments, const tb_ap *);
    size_t text_size = 0;
    char *text = NULL;
    tb_xp_digit *scratch = NULL;
    size_t required = 0;
    size_t digit_count = 0;
    size_t zeros = 0;
    size_t body = 0;
    size_t padding = 0;
    char sign = '\0';
    tb_xp_result written = TB_XP_OK;

    TB_ASSERT(output != NULL && spec != NULL);
    TB_ASSERT_MSG(x != NULL, "the argument of an arbitrary-precision conversion is NULL");
    text_size = tb_xp_string_size(x->length, 10);
    text = tb_allocator_alloc(&x->allocator, text_size, 1);
    scratch = ap_scratch(&x->allocator, x->length);
    written =
        tb_xp_to_string(x->length, ap_const_digits(x), 10, scratch, text, text_size, &required);
    TB_ASSERT(written == TB_XP_OK);
    ap_scratch_free(&x->allocator, scratch, x->length);

    /* As for %d: a precision of zero writes no digit for the value zero. */
    digit_count = required - 1;
    if (spec->has_precision && spec->precision == 0 && tb_ap_sign(x) == 0)
    {
        digit_count = 0;
    }
    sign = ap_sign_character(x, spec);
    zeros =
        spec->has_precision && spec->precision > digit_count ? spec->precision - digit_count : 0;
    body = (sign != '\0' ? 1u : 0u) + zeros + digit_count;
    padding = spec->has_width && spec->width > body ? spec->width - body : 0;

    if (!spec->left_justify && !(spec->zero_pad && !spec->has_precision))
    {
        ap_put_repeated(output, ' ', padding);
    }
    if (sign != '\0')
    {
        tb_fmt_put(output, &sign, 1);
    }
    if (!spec->left_justify && spec->zero_pad && !spec->has_precision)
    {
        ap_put_repeated(output, '0', padding);
    }
    ap_put_repeated(output, '0', zeros);
    tb_fmt_put(output, text, digit_count);
    if (spec->left_justify)
    {
        ap_put_repeated(output, ' ', padding);
    }
    tb_allocator_free(&x->allocator, text, text_size, 1);
}
