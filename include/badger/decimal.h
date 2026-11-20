/** @file decimal.h
 * @brief Exact conversion between `double` and text: the shortest text that
 * reads back as the same value, and reading text into the nearest value.
 *
 * Saved data, JSON, and network messages carry floating-point numbers as
 * text, and a number must come back with the very bits it was written with,
 * on every platform. The C library cannot promise that: printf and strtod
 * differ between platforms in rounding, in the spelling of infinity, and in
 * what they accept, and they follow the process locale. These functions
 * compute exactly and give the same bytes everywhere: the shortest text
 * with the Schubfach algorithm and a table of powers of ten, and reading
 * with big integers (xp.h).
 *
 * @code
 * char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
 * size_t required = 0;
 * double back = 0.0;
 *
 * (void)tb_decimal_shortest(0.1, text, sizeof(text), &required); // "0.1"
 * (void)tb_decimal_parse(text, required - 1, &back);              // back == 0.1
 * @endcode
 *
 * The formatter (fmt.h) uses the same arithmetic for `%f`, `%e`, `%g`, and
 * `%a`, which write a fixed number of digits as C specifies. This header is
 * for text that must read back as the same value, and for reading.
 *
 * **Not dependent on the rounding mode or the locale.** Conversions round to
 * nearest, ties to even, and the decimal point is always `.`.
 *
 * @par Threading
 * Thread-safe: the functions keep no state.
 */
#ifndef TB_DECIMAL_H
#define TB_DECIMAL_H

#include "badger/api.h"

#include <stddef.h>

/** @brief A buffer size that holds the text of tb_decimal_shortest() for any
 * value, terminator included. */
#define TB_DECIMAL_SHORTEST_SIZE 32

/** @brief How a conversion failed. */
typedef enum tb_decimal_result
{
    TB_DECIMAL_OK = 0,          /**< Success. */
    TB_DECIMAL_INVALID,         /**< The text is not a number of a form that is read. */
    TB_DECIMAL_BUFFER_TOO_SMALL /**< The buffer cannot hold the text; nothing was written. */
} tb_decimal_result;

/**
 * @brief Name a result for a log or a message.
 * @param result Any value.
 * @return A short phrase in lower case with static storage duration;
 * `"unknown result"` for a value that is no result.
 */
TB_API const char *tb_decimal_result_name(tb_decimal_result result);

/**
 * @brief Write the shortest text that reads back as exactly the same value.
 *
 * Of all the decimal numbers that round to `value`, the one with the fewest
 * significant digits is written, and of those the nearest to `value`. The
 * form is that of Python's `repr`, which is valid JSON for every finite
 * value:
 *
 * | Value | Text |
 * | --- | --- |
 * | 1, 0.1, 1234.5 | `1.0`, `0.1`, `1234.5`: at least one digit after the point |
 * | 1e16 and more, less than 1e-4 | `1e+16`, `1.5e-07`: an exponent of at least two digits |
 * | -0.0 | `-0.0` |
 * | infinities, NaN | `inf`, `-inf`, `nan`; NaN has no sign |
 *
 * Fixed notation is used when the decimal exponent of the first digit is
 * from -4 to 15.
 *
 * @param value Any value.
 * @param[out] buffer Receives the text and a terminating NUL; required
 * non-null unless `size` is 0.
 * @param size Size of `buffer` in bytes; #TB_DECIMAL_SHORTEST_SIZE is
 * always enough.
 * @param[out] required Receives the size the text needs, terminator
 * included, on success and when the buffer is too small; required non-null.
 * @return #TB_DECIMAL_OK, or #TB_DECIMAL_BUFFER_TOO_SMALL with nothing
 * written.
 */
TB_API tb_decimal_result tb_decimal_shortest(double value, char *buffer, size_t size,
                                             size_t *required);

/**
 * @brief Read text as the nearest `double`.
 *
 * The whole text must be one number, with no space before or after:
 *
 * - an optional `+` or `-`;
 * - then decimal digits with an optional `.` and at least one digit, and an
 *   optional exponent: `e` or `E`, an optional sign, and digits;
 * - or `0x` or `0X`, hexadecimal digits with an optional `.` and at least one
 *   digit, and an optional binary exponent: `p` or `P`, an optional sign, and
 *   decimal digits;
 * - or `inf`, `infinity`, or `nan`, in any case.
 *
 * Every digit counts, however many there are: the result is the `double`
 * nearest to the exact value of the text, ties to even. A value too large for
 * a `double` reads as infinity and a value too small as zero, each with the
 * sign of the text, as IEEE 754 rounding gives; neither is an error.
 *
 * @param text The text, not terminated; may be NULL only when `length` is 0.
 * @param length Number of bytes of text.
 * @param[out] value Receives the value on success; unchanged on failure.
 * Required non-null.
 * @return #TB_DECIMAL_OK, or #TB_DECIMAL_INVALID for text of any other form.
 */
TB_API tb_decimal_result tb_decimal_parse(const char *text, size_t length, double *value);

#endif
