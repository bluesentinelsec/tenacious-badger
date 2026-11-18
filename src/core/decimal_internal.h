/* The exact decimal digits of a double, shared by decimal.c and the
 * floating-point conversions of fmt.c. Private to the engine. */
#ifndef TB_DECIMAL_INTERNAL_H
#define TB_DECIMAL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most significant digits that the exact value of a double, or of a
 * boundary halfway between two doubles, can have: 769. */
#define TB_DECIMAL_EXACT_CAPACITY ((size_t)800)

/* A non-negative decimal number: digits[0].digits[1]digits[2]... times
 * 10^exponent. The digits are characters '0' to '9'; the first is not '0'
 * and the last is not '0'. Zero has no digits and an exponent of 0. */
typedef struct tb_decimal_exact
{
    char digits[TB_DECIMAL_EXACT_CAPACITY];
    size_t count;
    int exponent;
} tb_decimal_exact;

/* The parts of a double. */
typedef struct tb_decimal_parts
{
    bool negative;     /* The sign bit. */
    bool is_infinite;  /* An infinity. */
    bool is_nan;       /* Not a number. */
    uint64_t mantissa; /* The value is mantissa * 2^exponent; 0 for zero. */
    int exponent;
    bool is_normal; /* The mantissa has its 53rd bit, and so the gap below is
                       half the gap above when it is exactly 2^52. */
} tb_decimal_parts;

tb_decimal_parts tb_decimal_split(double value);

/* The exact digits of mantissa * 2^exponent, for a mantissa of at most 56
 * bits and an exponent from -1077 to 971. */
void tb_decimal_exact_of(uint64_t mantissa, int exponent, tb_decimal_exact *exact);

/* Rounds to `significant` digits, ties to even, which may carry into a new
 * first digit. A count of 0 or less keeps no digit: the result is zero, or
 * one unit of the place just above the first digit when the value is more
 * than half of it. */
void tb_decimal_round(tb_decimal_exact *exact, ptrdiff_t significant);

/* The digit at the place of 10^place, as a character; '0' beyond the digits. */
char tb_decimal_digit_at(const tb_decimal_exact *exact, ptrdiff_t place);

#endif
