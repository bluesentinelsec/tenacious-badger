/** @file ap.h
 * @brief Arbitrary-precision arithmetic: signed integers that grow as needed.
 *
 * A #tb_ap is an integer of any size: as many digits as its value needs, and
 * a sign. Every operation leaves its operands as they are and returns a new
 * value, which the caller owns and releases with tb_ap_free().
 *
 * @code
 * tb_ap *const two = tb_ap_from_i64(NULL, 2);
 * tb_ap *const exponent = tb_ap_from_i64(NULL, 100);
 * tb_ap *const power = tb_ap_pow(two, exponent, NULL);
 * char text[48] = {0};
 * size_t required = 0;
 *
 * (void)tb_ap_to_string(power, 10, text, sizeof(text), &required);
 * // text is "1267650600228229401496703205376"
 * tb_ap_free(power);
 * tb_ap_free(exponent);
 * tb_ap_free(two);
 * @endcode
 *
 * **Division rounds toward negative infinity.** tb_ap_div_floor() rounds the
 * quotient down, not toward zero as C's `/` does, and tb_ap_mod_floor()
 * returns the remainder that goes with it, which has the sign of the divisor:
 * -7 divided by 2 is -4 remainder 1, where C gives -3 remainder -1. The
 * remainder of a positive divisor is never negative, which is what modular
 * arithmetic needs. tb_ap_shift_right() rounds the same way. The names say so,
 * so that the choice is visible where it is used.
 *
 * **Memory.** A value gets its memory from an allocator (allocator.h), in one
 * block. A value made from a native integer or text uses the allocator it is
 * given; the result of an operation uses the allocator of its first operand,
 * and so does working space that the operation needs for a moment. Running
 * out of memory is fatal, as everywhere in the engine: an operation never
 * returns NULL. A result too large for memory, such as 2 to a power of
 * billions, therefore ends the process; bound the operands that come from
 * outside.
 *
 * **Values are never changed** once they are made, so a value may be an
 * operand of any number of operations, including both operands of one.
 *
 * **Contract violations** fail an assertion: a NULL value, division by zero,
 * a negative exponent, a modulus that is not positive, a base of text outside
 * 2 to 36. Malformed text and small buffers are inputs, and are returned as a
 * #tb_ap_result.
 *
 * **Not for cryptography.** How long an operation takes depends on the values
 * of its operands, and memory is not cleared when it is released.
 *
 * @par Threading
 * As thread-safe as the allocators of the values: thread-safe on the engine's
 * heap, externally synchronized on an arena. Since values never change, any
 * number of threads may use one value at once. tb_ap_free() is externally
 * synchronized with every other use of the value it releases.
 */
#ifndef TB_AP_H
#define TB_AP_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/fmt.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A signed integer of any size. Made by the functions of this header;
 * owned by the caller; released by tb_ap_free().
 */
typedef struct tb_ap tb_ap;

/** @brief How a conversion from or to text failed. */
typedef enum tb_ap_result
{
    TB_AP_OK = 0,          /**< Success. */
    TB_AP_INVALID,         /**< The text is not an optional sign followed by digits. */
    TB_AP_BUFFER_TOO_SMALL /**< The buffer cannot hold the text; nothing was written. */
} tb_ap_result;

/**
 * @brief Name a result for a log or a message.
 * @param result Any value.
 * @return A short phrase in lower case with static storage duration;
 * `"unknown result"` for a value that is no result.
 */
TB_API const char *tb_ap_result_name(tb_ap_result result);

/* -------------------------------------------------------------------------
 * Making and releasing values.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make a value from a signed native integer.
 * @param allocator Where the value gets its memory, or NULL for the engine's
 * heap.
 * @param value Any value, INT64_MIN included.
 * @return The new value; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_ap *tb_ap_from_i64(const tb_allocator *allocator, int64_t value);

/**
 * @brief Make a value from an unsigned native integer.
 * @param allocator Where the value gets its memory, or NULL for the heap.
 * @param value Any value.
 * @return The new value; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_ap *tb_ap_from_u64(const tb_allocator *allocator, uint64_t value);

/**
 * @brief Make a value from text.
 *
 * The text is an optional `+` or `-`, then at least one digit, and nothing
 * else: no space, no prefix such as `0x`. Letters stand for the digits from
 * 10 up, in either case. Leading zeros are allowed, and `-0` is zero.
 *
 * @param allocator Where the value gets its memory, or NULL for the heap.
 * @param text The text, not terminated; may be NULL only when `length` is 0.
 * @param length Number of bytes of text.
 * @param base The base of the digits, from 2 to 36.
 * @param[out] value Receives the new value on success, which the caller owns;
 * unchanged on failure. Required non-null.
 * @return #TB_AP_OK, or #TB_AP_INVALID for text of any other form.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_ap_result tb_ap_from_string(const tb_allocator *allocator, const char *text,
                                      size_t length, unsigned base, tb_ap **value);

/**
 * @brief Make a copy of a value.
 * @param x Required non-null.
 * @param allocator Where the copy gets its memory, or NULL for the heap.
 * @return The copy; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_ap *tb_ap_copy(const tb_ap *x, const tb_allocator *allocator);

/**
 * @brief Release a value, giving its memory back to its allocator.
 * @param x The value; NULL is ignored. Not valid afterwards.
 */
TB_API void tb_ap_free(tb_ap *x);

/* -------------------------------------------------------------------------
 * Converting values.
 * ------------------------------------------------------------------------- */

/**
 * @brief Convert a value to a signed native integer.
 * @param x Required non-null.
 * @param[out] value Receives the value when it fits; unchanged otherwise.
 * Required non-null.
 * @return true when the value is from INT64_MIN to INT64_MAX.
 */
TB_API bool tb_ap_to_i64(const tb_ap *x, int64_t *value);

/**
 * @brief Convert a value to an unsigned native integer.
 * @param x Required non-null.
 * @param[out] value Receives the value when it fits; unchanged otherwise.
 * Required non-null.
 * @return true when the value is from 0 to UINT64_MAX.
 */
TB_API bool tb_ap_to_u64(const tb_ap *x, uint64_t *value);

/**
 * @brief Write a value as text: a `-` for a negative value, then the digits
 * in small letters with no leading zeros.
 * @param x Required non-null.
 * @param base The base of the text, from 2 to 36.
 * @param[out] buffer Receives the text and a terminating NUL; required
 * non-null unless `size` is 0.
 * @param size Size of `buffer` in bytes. tb_ap_string_size() gives a size that
 * is always enough.
 * @param[out] required Receives the size the text needs, terminator included,
 * on success and when the buffer is too small; required non-null.
 * @return #TB_AP_OK, or #TB_AP_BUFFER_TOO_SMALL with nothing written.
 * @par Errors Exhaustion of working space calls the memory failure handler.
 */
TB_API tb_ap_result tb_ap_to_string(const tb_ap *x, unsigned base, char *buffer, size_t size,
                                    size_t *required);

/**
 * @brief A buffer size that holds the text of a value, sign and terminator
 * included. It may be a little more than the text needs, never less.
 * @param x Required non-null.
 * @param base The base of the text, from 2 to 36.
 */
TB_API size_t tb_ap_string_size(const tb_ap *x, unsigned base);

/**
 * @brief A conversion for the formatter (fmt.h) that writes a value in
 * decimal, to register with tb_fmt_register() under a letter of your choice.
 *
 * @code
 * (void)tb_fmt_register('Z', tb_ap_format);
 * tb_fmt_print("%Z apples, %+08Z pears\n", apples, pears);
 * @endcode
 *
 * Its argument is a `const tb_ap *`, required non-null. It follows the rules
 * of `%d`: the `+` and space flags for the sign of a non-negative value, a
 * precision for the least number of digits, a width, and the `-` and `0`
 * flags for how to fill it. The text is built in working space from the
 * value's allocator.
 *
 * @param output Where to write; given by the formatter.
 * @param spec The flags, width, and precision; given by the formatter.
 * @param arguments The arguments of the format call; one is taken.
 */
TB_API void tb_ap_format(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments);

/* -------------------------------------------------------------------------
 * Arithmetic. Each returns a new value from the allocator of its first
 * operand; never NULL. Exhaustion calls the memory failure handler.
 * ------------------------------------------------------------------------- */

/** @brief -x. @param x Required non-null. */
TB_API tb_ap *tb_ap_neg(const tb_ap *x);

/** @brief x + y. @param x Required non-null. @param y Required non-null. */
TB_API tb_ap *tb_ap_add(const tb_ap *x, const tb_ap *y);

/** @brief x - y. @param x Required non-null. @param y Required non-null. */
TB_API tb_ap *tb_ap_sub(const tb_ap *x, const tb_ap *y);

/** @brief x * y. @param x Required non-null. @param y Required non-null. */
TB_API tb_ap *tb_ap_mul(const tb_ap *x, const tb_ap *y);

/**
 * @brief x / y, rounded toward negative infinity.
 * @param x Required non-null.
 * @param y Required non-null and not zero.
 */
TB_API tb_ap *tb_ap_div_floor(const tb_ap *x, const tb_ap *y);

/**
 * @brief The remainder that goes with tb_ap_div_floor(): x - y * floor(x / y),
 * which is zero or has the sign of `y`.
 * @param x Required non-null.
 * @param y Required non-null and not zero.
 */
TB_API tb_ap *tb_ap_mod_floor(const tb_ap *x, const tb_ap *y);

/**
 * @brief x to the power y, or x to the power y modulo p.
 *
 * Square and multiply, with the modulus applied at every step when there is
 * one, so that intermediate values stay smaller than p squared. Zero to the
 * power zero is one.
 *
 * @param x Required non-null; any sign.
 * @param y The exponent; required non-null and not negative.
 * @param p The modulus, or NULL for none. When given, it is positive and the
 * result is from 0 to p - 1, as tb_ap_mod_floor() gives.
 */
TB_API tb_ap *tb_ap_pow(const tb_ap *x, const tb_ap *y, const tb_ap *p);

/** @brief x * 2^bits. @param x Required non-null. @param bits Any count. */
TB_API tb_ap *tb_ap_shift_left(const tb_ap *x, size_t bits);

/**
 * @brief x / 2^bits, rounded toward negative infinity, as an arithmetic shift
 * of a two's complement integer is: -1 shifted right is still -1.
 * @param x Required non-null.
 * @param bits Any count.
 */
TB_API tb_ap *tb_ap_shift_right(const tb_ap *x, size_t bits);

/* -------------------------------------------------------------------------
 * Arithmetic with a native integer as the second operand. Each is the same
 * as its counterpart above with tb_ap_from_i64(y) as `y`, without making it.
 * ------------------------------------------------------------------------- */

/** @brief x + y. @param x Required non-null. @param y Any value. */
TB_API tb_ap *tb_ap_add_i64(const tb_ap *x, int64_t y);

/** @brief x - y. @param x Required non-null. @param y Any value. */
TB_API tb_ap *tb_ap_sub_i64(const tb_ap *x, int64_t y);

/** @brief x * y. @param x Required non-null. @param y Any value. */
TB_API tb_ap *tb_ap_mul_i64(const tb_ap *x, int64_t y);

/**
 * @brief x / y, rounded toward negative infinity.
 * @param x Required non-null.
 * @param y Not zero.
 */
TB_API tb_ap *tb_ap_div_floor_i64(const tb_ap *x, int64_t y);

/**
 * @brief x - y * floor(x / y), which is zero or has the sign of `y`. It
 * always fits in the type of `y`, so it is returned as a native integer.
 * @param x Required non-null.
 * @param y Not zero.
 * @par Errors Exhaustion of working space calls the memory failure handler.
 */
TB_API int64_t tb_ap_mod_floor_i64(const tb_ap *x, int64_t y);

/* -------------------------------------------------------------------------
 * Comparing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Compare two values.
 * @param x Required non-null.
 * @param y Required non-null.
 * @return Less than 0, 0, or more than 0 as `x` is less than, equal to, or
 * greater than `y`.
 */
TB_API int tb_ap_compare(const tb_ap *x, const tb_ap *y);

/** @brief tb_ap_compare() with a native integer. @param x Required non-null. @param y Any value. */
TB_API int tb_ap_compare_i64(const tb_ap *x, int64_t y);

/**
 * @brief The sign of a value.
 * @param x Required non-null.
 * @return -1, 0, or 1.
 */
TB_API int tb_ap_sign(const tb_ap *x);

#endif
