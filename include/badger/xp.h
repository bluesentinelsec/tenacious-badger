/** @file xp.h
 * @brief Extended-precision arithmetic: unsigned integers of a fixed number of
 * digits, in arrays that the caller supplies.
 *
 * Native integers stop at 64 bits. These functions add, subtract, multiply,
 * divide, shift, compare, and convert unsigned integers of any fixed length,
 * held in arrays of digits that the caller owns. Nothing is allocated, so the
 * functions suit an arena, a stack buffer, or a big integer type built on top
 * of them, such as the arbitrary-precision integers of ap.h.
 *
 * @code
 * tb_xp_digit left[4] = {0};
 * tb_xp_digit right[4] = {0};
 * tb_xp_digit product[8] = {0};
 * char text[80] = {0};
 * tb_xp_digit scratch[8] = {0};
 * size_t required = 0;
 *
 * (void)tb_xp_from_u64(4, left, UINT64_MAX);
 * (void)tb_xp_from_u64(4, right, UINT64_MAX);
 * tb_xp_mul(product, 4, left, 4, right);
 * (void)tb_xp_to_string(8, product, 10, scratch, text, sizeof(text), &required);
 * @endcode
 *
 * **Digits.** A number of `n` digits is an array of `n` #tb_xp_digit values,
 * each a digit in base 2^32, the least significant first: the value of `x` is
 * `x[0] + x[1] * 2^32 + ... + x[n-1] * 2^(32(n-1))`. Every length is at least
 * one digit.
 *
 * **Carries and borrows.** Where a result can be wider than its operands, the
 * function writes the low digits and returns what did not fit: a carry or a
 * borrow of 0 or 1, or a whole digit. Chaining calls through these returns
 * builds arithmetic on longer numbers.
 *
 * **Aliasing.** A function documents which of its outputs may be the same
 * array as which of its inputs. "May be the same array" means exactly the same
 * address and length; a partial overlap is never allowed. Builds with
 * assertions check the arguments that must not overlap.
 *
 * **Not for cryptography.** How long an operation takes depends on the values
 * of its operands, and memory is not cleared after use. Do not use these
 * functions for secret keys or anything else that must not leak through
 * timing.
 *
 * **Contract violations** fail an assertion: a null pointer, a length of
 * zero, a base outside 2 to 36, division by zero, arrays that overlap where
 * they must not. Text and buffer sizes are inputs, and their failures are
 * returned as a #tb_xp_result.
 *
 * @par Threading
 * Thread-safe: the functions keep no state. Two threads that use the same
 * output array, or one writing an array that another reads, are externally
 * synchronized by the caller.
 */
#ifndef TB_XP_H
#define TB_XP_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief One digit of an extended-precision number, in base 2^32. */
typedef uint32_t tb_xp_digit;

/** @brief The number of bits in a #tb_xp_digit. */
#define TB_XP_DIGIT_BITS 32

/** @brief The smallest base of text that the string conversions accept. */
#define TB_XP_BASE_MIN 2u

/** @brief The largest base of text that the string conversions accept: digits
 * `0` to `9`, then `a` to `z`. */
#define TB_XP_BASE_MAX 36u

/**
 * @brief The number of scratch digits that tb_xp_div() needs.
 * @param n Number of digits of the dividend.
 * @param m Number of digits of the divisor.
 */
#define TB_XP_DIV_SCRATCH(n, m) ((n) + (m) + 1)

/** @brief How a conversion from or to text failed. */
typedef enum tb_xp_result
{
    TB_XP_OK = 0,          /**< Success. */
    TB_XP_INVALID,         /**< The text is empty or holds a character that is not a digit. */
    TB_XP_TOO_LARGE,       /**< The value does not fit in the digits given. */
    TB_XP_BUFFER_TOO_SMALL /**< The buffer cannot hold the text; nothing was written. */
} tb_xp_result;

/**
 * @brief Name a result for a log or a message.
 * @param result Any value.
 * @return A short phrase in lower case with static storage duration, such as
 * `"too large"`; `"unknown result"` for a value that is no result.
 */
TB_API const char *tb_xp_result_name(tb_xp_result result);

/* -------------------------------------------------------------------------
 * Addition and subtraction.
 * ------------------------------------------------------------------------- */

/**
 * @brief Add: z = x + y + carry.
 * @param n Number of digits of each array; more than 0.
 * @param[out] z Receives the low `n` digits of the sum; required non-null. It
 * may be the same array as `x`, `y`, or both.
 * @param x First operand; required non-null.
 * @param y Second operand; required non-null.
 * @param carry A carry into the lowest digit: 0 or 1.
 * @return The carry out of the highest digit: 0 or 1.
 */
TB_API unsigned tb_xp_add(size_t n, tb_xp_digit *z, const tb_xp_digit *x, const tb_xp_digit *y,
                          unsigned carry);

/**
 * @brief Subtract: z = x - y - borrow.
 * @param n Number of digits of each array; more than 0.
 * @param[out] z Receives the low `n` digits of the difference, which wraps
 * around modulo 2^(32n) when it is negative; required non-null. It may be the
 * same array as `x`, `y`, or both.
 * @param x Minuend; required non-null.
 * @param y Subtrahend; required non-null.
 * @param borrow A borrow from the lowest digit: 0 or 1.
 * @return The borrow from beyond the highest digit: 1 when the difference is
 * negative, otherwise 0.
 */
TB_API unsigned tb_xp_sub(size_t n, tb_xp_digit *z, const tb_xp_digit *x, const tb_xp_digit *y,
                          unsigned borrow);

/**
 * @brief Complement and add: z = ~x + carry, where ~x inverts every bit.
 *
 * With a carry of 1 this is the two's complement negation, 2^(32n) - x.
 *
 * @param n Number of digits of each array; more than 0.
 * @param[out] z Receives the result; required non-null. It may be the same
 * array as `x`.
 * @param x Operand; required non-null.
 * @param carry 0 or 1.
 * @return The carry out of the highest digit: 1 only when `x` is zero and
 * `carry` is 1.
 */
TB_API unsigned tb_xp_neg(size_t n, tb_xp_digit *z, const tb_xp_digit *x, unsigned carry);

/**
 * @brief Add a single digit: z = x + y.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the low `n` digits of the sum; required non-null. It
 * may be the same array as `x`.
 * @param x Operand; required non-null.
 * @param y The digit to add.
 * @return The carry out of the highest digit: 0 or 1.
 */
TB_API unsigned tb_xp_sum(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y);

/**
 * @brief Subtract a single digit: z = x - y.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the low `n` digits of the difference, wrapping
 * around when it is negative; required non-null. It may be the same array as
 * `x`.
 * @param x Operand; required non-null.
 * @param y The digit to subtract.
 * @return 1 when the difference is negative, otherwise 0.
 */
TB_API unsigned tb_xp_diff(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y);

/* -------------------------------------------------------------------------
 * Multiplication and division.
 * ------------------------------------------------------------------------- */

/**
 * @brief Multiply: z = x * y.
 * @param[out] z Receives the product in `n + m` digits, which always hold it;
 * required non-null. It must not overlap `x` or `y`.
 * @param n Number of digits of `x`; more than 0.
 * @param x First operand; required non-null.
 * @param m Number of digits of `y`; more than 0.
 * @param y Second operand; required non-null. It may be the same array as
 * `x`, to square a number.
 */
TB_API void tb_xp_mul(tb_xp_digit *z, size_t n, const tb_xp_digit *x, size_t m,
                      const tb_xp_digit *y);

/**
 * @brief Multiply by a single digit: z = x * y.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the low `n` digits of the product; required non-null.
 * It may be the same array as `x`.
 * @param x Operand; required non-null.
 * @param y The digit to multiply by.
 * @return The digit that the product carries beyond `n` digits.
 */
TB_API tb_xp_digit tb_xp_product(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y);

/**
 * @brief Divide: q = x / y and r = x % y, rounding the quotient down.
 *
 * Long division, after Knuth's Algorithm D, in time proportional to the
 * product of the significant lengths.
 *
 * @param n Number of digits of `x` and `q`; more than 0.
 * @param[out] q Receives the quotient; required non-null.
 * @param x Dividend; required non-null.
 * @param m Number of digits of `y` and `r`; more than 0. It may be more or
 * fewer than `n`.
 * @param y Divisor; required non-null and not zero.
 * @param[out] r Receives the remainder, which is less than `y` and so always
 * fits; required non-null.
 * @param[out] scratch Working space of TB_XP_DIV_SCRATCH(n, m) digits,
 * required non-null; its contents afterwards are unspecified.
 *
 * None of `q`, `r`, and `scratch` may overlap one another, `x`, or `y`.
 */
TB_API void tb_xp_div(size_t n, tb_xp_digit *q, const tb_xp_digit *x, size_t m,
                      const tb_xp_digit *y, tb_xp_digit *r, tb_xp_digit *scratch);

/**
 * @brief Divide by a single digit: z = x / y.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the quotient, rounded down; required non-null. It
 * may be the same array as `x`.
 * @param x Dividend; required non-null.
 * @param y The divisor; not zero.
 * @return The remainder, x % y.
 */
TB_API tb_xp_digit tb_xp_quotient(size_t n, tb_xp_digit *z, const tb_xp_digit *x, tb_xp_digit y);

/* -------------------------------------------------------------------------
 * Comparing and shifting.
 * ------------------------------------------------------------------------- */

/**
 * @brief Compare two numbers of the same length.
 * @param n Number of digits of each; more than 0.
 * @param x Required non-null.
 * @param y Required non-null.
 * @return Less than 0, 0, or more than 0 as `x` is less than, equal to, or
 * greater than `y`.
 */
TB_API int tb_xp_compare(size_t n, const tb_xp_digit *x, const tb_xp_digit *y);

/**
 * @brief Shift left: z = x * 2^bits, keeping the low `n` digits.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the result; required non-null. It may be the same
 * array as `x`.
 * @param x Operand; required non-null.
 * @param bits Any count. Bits shifted beyond the highest digit are lost, and
 * a count of 32n or more leaves zero.
 */
TB_API void tb_xp_shift_left(size_t n, tb_xp_digit *z, const tb_xp_digit *x, size_t bits);

/**
 * @brief Shift right: z = x / 2^bits, rounded down.
 * @param n Number of digits of `z` and `x`; more than 0.
 * @param[out] z Receives the result; required non-null. It may be the same
 * array as `x`.
 * @param x Operand; required non-null.
 * @param bits Any count. A count of 32n or more leaves zero.
 */
TB_API void tb_xp_shift_right(size_t n, tb_xp_digit *z, const tb_xp_digit *x, size_t bits);

/**
 * @brief The significant length of a number: the digits up to and including
 * the highest one that is not zero.
 * @param n Number of digits; more than 0.
 * @param x Required non-null.
 * @return A count from 1 to `n`; 1 when the value is zero, so that the result
 * is always a valid length for the other functions.
 */
TB_API size_t tb_xp_length(size_t n, const tb_xp_digit *x);

/* -------------------------------------------------------------------------
 * Conversions.
 * ------------------------------------------------------------------------- */

/**
 * @brief Set a number from a native integer.
 * @param n Number of digits of `z`; more than 0.
 * @param[out] z Receives the value, or its low `n` digits when it does not
 * fit; required non-null.
 * @param value The value.
 * @return true when the whole value fit.
 */
TB_API bool tb_xp_from_u64(size_t n, tb_xp_digit *z, uint64_t value);

/**
 * @brief Convert a number to a native integer.
 * @param n Number of digits of `x`; more than 0.
 * @param x Required non-null.
 * @param[out] value Receives the low 64 bits of the number, whether or not it
 * fits; required non-null.
 * @return true when the number fits in 64 bits.
 */
TB_API bool tb_xp_to_u64(size_t n, const tb_xp_digit *x, uint64_t *value);

/**
 * @brief Set a number from its digits as text.
 *
 * The text is digits and nothing else: no sign, no prefix such as `0x`, no
 * space. Letters stand for the digits from 10 up, in either case. Leading
 * zeros are allowed.
 *
 * @param n Number of digits of `z`; more than 0.
 * @param[out] z Receives the value; required non-null. Unchanged when the
 * result is #TB_XP_INVALID; unspecified when it is #TB_XP_TOO_LARGE.
 * @param text The text, not terminated; may be NULL only when `length` is 0.
 * @param length Number of bytes of text.
 * @param base The base of the digits, from #TB_XP_BASE_MIN to #TB_XP_BASE_MAX.
 * @return #TB_XP_OK; #TB_XP_INVALID when the text is empty or holds a byte
 * that is not a digit of `base`; #TB_XP_TOO_LARGE when the value needs more
 * than `n` digits.
 */
TB_API tb_xp_result tb_xp_from_string(size_t n, tb_xp_digit *z, const char *text, size_t length,
                                      unsigned base);

/**
 * @brief Write a number as text, with small letters for digits from 10 up and
 * no leading zeros.
 * @param n Number of digits of `x` and `scratch`; more than 0.
 * @param x The number; required non-null.
 * @param base The base of the text, from #TB_XP_BASE_MIN to #TB_XP_BASE_MAX.
 * @param[out] scratch Working space of `n` digits, required non-null, that
 * must not overlap `x`; its contents afterwards are unspecified.
 * @param[out] buffer Receives the text and a terminating NUL; required
 * non-null unless `size` is 0.
 * @param size Size of `buffer` in bytes. tb_xp_string_size() gives a size that
 * is always enough.
 * @param[out] required Receives the size the text needs, terminator included,
 * on success and when the buffer is too small; required non-null.
 * @return #TB_XP_OK, or #TB_XP_BUFFER_TOO_SMALL with nothing written to
 * `buffer`.
 */
TB_API tb_xp_result tb_xp_to_string(size_t n, const tb_xp_digit *x, unsigned base,
                                    tb_xp_digit *scratch, char *buffer, size_t size,
                                    size_t *required);

/**
 * @brief A buffer size that holds the text of any number of `n` digits.
 * @param n Number of digits; more than 0, and at most SIZE_MAX / 64.
 * @param base The base of the text, from #TB_XP_BASE_MIN to #TB_XP_BASE_MAX.
 * @return A size in bytes, terminator included. It is never less than the
 * text of the largest number of `n` digits needs, and exceeds that by at most
 * 2 + n / 2048 bytes.
 */
TB_API size_t tb_xp_string_size(size_t n, unsigned base);

#endif
