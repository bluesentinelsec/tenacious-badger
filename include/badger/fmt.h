/** @file fmt.h
 * @brief Formatted output: printf-style formatting that writes anywhere and
 * can be taught new conversions.
 *
 * Logging, the console, error messages, and string building all format text.
 * This is the one implementation they share. It does not use the C library's
 * printf, so a format produces the same bytes on every platform and never
 * depends on the process locale.
 *
 * @code
 * char label[32] = {0};
 * char *message = NULL;
 *
 * (void)tb_fmt_buffer(label, sizeof(label), "%s #%03d", name, index);
 * message = tb_fmt_string(NULL, "loaded %zu of %zu assets", loaded, total);
 * tb_fmt_print("%s\n", message);
 * tb_str_free(NULL, message);
 * @endcode
 *
 * **Where the output goes.** tb_fmt_buffer() writes into a buffer the caller
 * supplies, tb_fmt_string() into a new string, tb_fmt_print() to standard
 * output, tb_fmt_print_error() to standard error, and tb_fmt_write() to any
 * function that accepts bytes. Each has a `v` form that takes a `va_list`.
 * tb_fmt_flush() delivers what has been printed to the standard streams.
 *
 * **Format strings.** A conversion is `%`, then optional flags, width,
 * precision, and length, then a conversion character, as in C:
 *
 * | Part | Values |
 * | --- | --- |
 * | Flags | `-` left-justify, `+` always show a sign, space for a space before non-negative
 * numbers, `0` pad with zeros, `#` alternate form | | Width | a number, or `*` to take an `int`
 * argument: the least number of bytes to write | | Precision | `.` and a number, or `.*`: the least
 * number of digits for an integer, or the most bytes of a string | | Length | `hh`, `h`, `l`, `ll`,
 * `j`, `z`, `t`: the type of an integer argument, as in C |
 *
 * | Conversion | Argument | Output |
 * | --- | --- | --- |
 * | `%d`, `%i` | signed integer | decimal |
 * | `%u` | unsigned integer | decimal |
 * | `%x`, `%X` | unsigned integer | hexadecimal, small or capital letters; `#` adds `0x` or `0X` |
 * | `%o` | unsigned integer | octal; `#` adds a leading `0` |
 * | `%c` | `int` | the byte with that value |
 * | `%s` | `const char *` | the string; `(null)` for a null pointer |
 * | `%p` | `void *` | `0x` and the address in hexadecimal |
 * | `%%` | none | a percent sign |
 *
 * Integer conversions behave exactly as the C standard specifies for printf.
 *
 * **Floating point is not converted yet.** `%f`, `%e`, `%g`, and `%a` are
 * reserved for a correct, platform-independent conversion, which is a project
 * of its own. Until then they are invalid.
 *
 * **An invalid format string is a contract violation.** A conversion
 * character that is not known, or a format that ends inside a conversion,
 * fails an assertion. In builds without assertions the invalid conversion is
 * written out as it stands. Format strings are part of the program, not input:
 * never pass text from outside as a format.
 *
 * **New conversions.** tb_fmt_register() adds a conversion character for a
 * type of your own, so that it can be formatted like any built-in one.
 *
 * @par Threading
 * The formatting functions are thread-safe; two threads writing to the same
 * destination must be serialized by the caller. tb_fmt_register() is
 * stage-owned: call it during single-threaded startup.
 */
#ifndef TB_FMT_H
#define TB_FMT_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Formatting to a destination.
 * ------------------------------------------------------------------------- */

/**
 * @brief Receives formatted output, some bytes at a time.
 * @param context The pointer given to tb_fmt_write().
 * @param bytes The bytes; valid for the duration of the call, not terminated.
 * @param length Number of bytes; more than 0.
 */
typedef void tb_fmt_sink(void *context, const char *bytes, size_t length);

/**
 * @brief Format to a function that accepts bytes.
 * @param sink Called with each run of output in order; required non-null.
 * @param context Passed to every call of `sink`; may be NULL.
 * @param format Format string; required non-null.
 * @param ... One argument for each conversion, of the type it expects.
 * @return The number of bytes written.
 */
TB_API size_t tb_fmt_write(tb_fmt_sink *sink, void *context, const char *format, ...);

/** @brief tb_fmt_write() with its arguments in a `va_list`, which is left as it was. */
TB_API size_t tb_fmt_vwrite(tb_fmt_sink *sink, void *context, const char *format,
                            va_list arguments);

/**
 * @brief Format into a buffer.
 *
 * The output is truncated when it does not fit, and the buffer is always
 * terminated. The return value tells the two cases apart and says how large
 * a buffer would have been enough.
 *
 * @param buffer Receives the output; required non-null unless `size` is 0.
 * @param size Size of the buffer in bytes, terminator included. 0 writes
 * nothing and only measures.
 * @param format Format string; required non-null.
 * @param ... One argument for each conversion.
 * @return The length of the whole output, not counting the terminator. It
 * fit when this is less than `size`.
 */
TB_API size_t tb_fmt_buffer(char *buffer, size_t size, const char *format, ...);

/** @brief tb_fmt_buffer() with its arguments in a `va_list`, which is left as it was. */
TB_API size_t tb_fmt_vbuffer(char *buffer, size_t size, const char *format, va_list arguments);

/**
 * @brief Format into a new string.
 * @param allocator Where the string comes from, or NULL for the engine's heap.
 * @param format Format string; required non-null. Short text is formatted
 * once; text of 256 bytes or more is processed twice, once to measure, so a
 * custom conversion must give the same output both times.
 * @param ... One argument for each conversion.
 * @return The NUL-terminated output, exactly as long as it needs to be; never
 * NULL. The caller owns it and releases it with tb_str_free() (str.h).
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API char *tb_fmt_string(const tb_allocator *allocator, const char *format, ...);

/** @brief tb_fmt_string() with its arguments in a `va_list`, which is left as it was. */
TB_API char *tb_fmt_vstring(const tb_allocator *allocator, const char *format, va_list arguments);

/**
 * @brief Format to standard output.
 * @param format Format string; required non-null.
 * @param ... One argument for each conversion.
 * @return The number of bytes written.
 */
TB_API size_t tb_fmt_print(const char *format, ...);

/** @brief tb_fmt_print() with its arguments in a `va_list`, which is left as it was. */
TB_API size_t tb_fmt_vprint(const char *format, va_list arguments);

/**
 * @brief Format to standard error.
 * @param format Format string; required non-null.
 * @param ... One argument for each conversion.
 * @return The number of bytes written.
 */
TB_API size_t tb_fmt_print_error(const char *format, ...);

/** @brief tb_fmt_print_error() with its arguments in a `va_list`, which is left as it was. */
TB_API size_t tb_fmt_vprint_error(const char *format, va_list arguments);

/**
 * @brief Deliver everything printed so far to standard output and standard error.
 *
 * Standard output may hold text back until there is enough of it to be worth
 * sending. Call this before anything that must find the text already
 * delivered: waiting for input, starting another program, or ending the
 * process abnormally. A failed assertion and an exhausted fail-fast allocation
 * call it before they end the process.
 *
 * The streams are the engine's own. Where the engine is a shared library with
 * a C library of its own, as on Windows, the `fflush(stdout)` of the program
 * that uses it does not reach them, and this function does.
 *
 * @return `true` when both streams have accepted everything written to them
 * so far; `false` once either has reported an error.
 * @par Threading Thread-safe.
 */
TB_API bool tb_fmt_flush(void);

/* -------------------------------------------------------------------------
 * Adding conversions.
 * ------------------------------------------------------------------------- */

/** @brief The length part of a conversion: the size of its integer argument. */
typedef enum tb_fmt_length
{
    TB_FMT_LENGTH_DEFAULT,   /**< None: `int`. */
    TB_FMT_LENGTH_CHAR,      /**< `hh`: `char`, passed as `int`. */
    TB_FMT_LENGTH_SHORT,     /**< `h`: `short`, passed as `int`. */
    TB_FMT_LENGTH_LONG,      /**< `l`: `long`. */
    TB_FMT_LENGTH_LONG_LONG, /**< `ll`: `long long`. */
    TB_FMT_LENGTH_MAX,       /**< `j`: `intmax_t`. */
    TB_FMT_LENGTH_SIZE,      /**< `z`: `size_t`. */
    TB_FMT_LENGTH_POINTER    /**< `t`: `ptrdiff_t`. */
} tb_fmt_length;

/** @brief One parsed conversion: everything between `%` and its character. */
typedef struct tb_fmt_spec
{
    char conversion;      /**< The conversion character. */
    bool left_justify;    /**< `-`: pad on the right instead of the left. */
    bool show_sign;       /**< `+`: write a sign for non-negative numbers too. */
    bool space_sign;      /**< Space: write a space before non-negative numbers. */
    bool zero_pad;        /**< `0`: pad numbers with zeros instead of spaces. */
    bool alternate;       /**< `#`: the alternate form of the conversion. */
    bool has_width;       /**< A width was given. */
    bool has_precision;   /**< A precision was given. */
    size_t width;         /**< The width; 0 unless `has_width`. */
    size_t precision;     /**< The precision; 0 unless `has_precision`. */
    tb_fmt_length length; /**< The length part. */
} tb_fmt_spec;

/** @brief Where a conversion writes its output. Private to the formatter. */
typedef struct tb_fmt_output tb_fmt_output;

/**
 * @brief A conversion: turns the next argument into text.
 *
 * It takes exactly the arguments it documents from `arguments` with `va_arg`,
 * and writes with tb_fmt_put() or tb_fmt_put_padded(). It must give the same
 * output every time it is called with the same arguments.
 *
 * @param output Where to write.
 * @param spec The flags, width, precision, and length of this conversion.
 * @param arguments The remaining arguments of the format call.
 */
typedef void tb_fmt_conversion(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments);

/**
 * @brief Add, replace, or remove the conversion for a character.
 *
 * @code
 * static void format_vector(tb_fmt_output *output, const tb_fmt_spec *spec, va_list *arguments)
 * {
 *     const vector *const value = va_arg(*arguments, const vector *);
 *     char text[64] = {0};
 *     const size_t length = tb_fmt_buffer(text, sizeof(text), "(%d, %d)", value->x, value->y);
 *
 *     tb_fmt_put_padded(output, spec, text, length);
 * }
 *
 * (void)tb_fmt_register('V', format_vector);
 * tb_fmt_print("moved to %V\n", &position);
 * @endcode
 *
 * @param conversion The character: an ASCII letter that is not a built-in
 * conversion (`d i u x X o c s p`), a reserved one (`a A e E f F g G n`), or a
 * length letter (`h l j z t L`).
 * @param function The conversion, or NULL to remove the one registered.
 * @return The function registered before, or NULL when there was none.
 * @par Threading Stage-owned: call during single-threaded startup.
 */
TB_API tb_fmt_conversion *tb_fmt_register(char conversion, tb_fmt_conversion *function);

/**
 * @brief Write bytes from a conversion, as they are.
 * @param output The output given to the conversion; required non-null.
 * @param bytes The bytes; may be NULL only when `length` is 0.
 * @param length Number of bytes.
 */
TB_API void tb_fmt_put(tb_fmt_output *output, const char *bytes, size_t length);

/**
 * @brief Write bytes from a conversion, padded with spaces to the width of
 * its spec and justified as its spec asks.
 * @param output The output given to the conversion; required non-null.
 * @param spec The spec given to the conversion; required non-null. Only its
 * width and `left_justify` are used.
 * @param bytes The bytes; may be NULL only when `length` is 0.
 * @param length Number of bytes.
 */
TB_API void tb_fmt_put_padded(tb_fmt_output *output, const tb_fmt_spec *spec, const char *bytes,
                              size_t length);

#endif
