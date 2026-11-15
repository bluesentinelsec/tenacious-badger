/** @file str.h
 * @brief Operations on C strings: searching, comparing, and building new ones.
 *
 * These functions work on NUL-terminated strings, the form the C library, the
 * operating system, and string literals use. They treat a string as bytes.
 *
 * **Positions are byte indexes from 0.** A part of a string is given as
 * `begin` and `end` and holds the bytes from `begin` up to, but not
 * including, `end`. Pass #TB_STR_END as `end` to mean the end of the string.
 * `0, TB_STR_END` is the whole string.
 *
 * **A search returns the index of what it found**, or #TB_STR_NOT_FOUND. The
 * backward searches, named `tb_str_rfind_*`, return the last match in the
 * range instead of the first.
 *
 * @code
 * const size_t dot = tb_str_rfind_char(file_name, 0, TB_STR_END, '.');
 *
 * if (dot != TB_STR_NOT_FOUND && tb_str_equal(file_name + dot, ".png"))
 * {
 *     char *const stem = tb_str_sub(NULL, file_name, 0, dot);
 *     ...
 *     tb_str_free(NULL, stem);
 * }
 * @endcode
 *
 * **Text is UTF-8, and these functions do not decode it.** UTF-8 is designed
 * so that bytewise searching for a valid string finds only real occurrences,
 * and bytewise comparison orders strings by code point, so searching,
 * comparing, joining, and repeating are correct for any UTF-8 text. Three
 * things are not: a position is a byte index, not a character count;
 * tb_str_sub() with a position inside a multi-byte character produces invalid
 * UTF-8; and tb_str_reverse() and tb_str_map() are for ASCII only.
 *
 * **New strings come from an allocator** (allocator.h), NULL for the engine's
 * heap. The functions that build a string never return NULL: exhaustion ends
 * the process, as everywhere in the engine. The caller owns the result and
 * releases it with tb_str_free(), or allocates it in an arena and releases
 * nothing.
 *
 * @par Threading
 * Thread-safe: no function keeps state. The caller ensures that a string is
 * not changed while another thread reads it.
 */
#ifndef TB_STR_H
#define TB_STR_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Returned by the search functions when nothing matches. */
#define TB_STR_NOT_FOUND SIZE_MAX

/** @brief As the `end` of a range: the end of the string. */
#define TB_STR_END SIZE_MAX

/* -------------------------------------------------------------------------
 * Measuring and comparing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Number of bytes in a string, not counting its terminator.
 * @param string Required non-null.
 */
TB_API size_t tb_str_length(const char *string);

/**
 * @brief Compare two strings byte by byte.
 * @param left Required non-null.
 * @param right Required non-null.
 * @return Negative, zero, or positive as `left` sorts before, equal to, or
 * after `right`. Bytes compare as unsigned values, which orders UTF-8 by code
 * point; a string that is a prefix of the other sorts first.
 */
TB_API int tb_str_compare(const char *left, const char *right);

/**
 * @brief Whether two strings have the same bytes.
 * @param left Required non-null.
 * @param right Required non-null.
 */
TB_API bool tb_str_equal(const char *left, const char *right);

/**
 * @brief Whether a string begins with another.
 * @param string Required non-null.
 * @param prefix Required non-null. Every string begins with the empty string.
 */
TB_API bool tb_str_starts_with(const char *string, const char *prefix);

/**
 * @brief Whether a string ends with another.
 * @param string Required non-null.
 * @param suffix Required non-null. Every string ends with the empty string.
 */
TB_API bool tb_str_ends_with(const char *string, const char *suffix);

/**
 * @brief Whether a pattern occurs in a string at a position.
 * @param string Required non-null.
 * @param position Where the pattern must begin; at most the length.
 * @param pattern Required non-null. The empty pattern occurs at every position.
 */
TB_API bool tb_str_matches_at(const char *string, size_t position, const char *pattern);

/* -------------------------------------------------------------------------
 * Searching a range of a string. `begin` is at most `end`, and `end` is at
 * most the length of the string or is TB_STR_END.
 * ------------------------------------------------------------------------- */

/**
 * @brief Find the first occurrence of a byte in a range.
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param character Byte to look for. The terminator is not part of the
 * string, so `'\0'` is never found.
 * @return Its index, or #TB_STR_NOT_FOUND.
 */
TB_API size_t tb_str_find_char(const char *string, size_t begin, size_t end, char character);

/** @brief Find the last occurrence of a byte in a range. See tb_str_find_char(). */
TB_API size_t tb_str_rfind_char(const char *string, size_t begin, size_t end, char character);

/**
 * @brief Find the first byte in a range that is one of a set.
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param set The bytes to look for, as a string; required non-null. An empty
 * set matches nothing.
 * @return The index of the byte, or #TB_STR_NOT_FOUND.
 */
TB_API size_t tb_str_find_any(const char *string, size_t begin, size_t end, const char *set);

/** @brief Find the last byte in a range that is one of a set. See tb_str_find_any(). */
TB_API size_t tb_str_rfind_any(const char *string, size_t begin, size_t end, const char *set);

/**
 * @brief Find the first occurrence of a pattern that lies wholly in a range.
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param pattern Required non-null. The empty pattern is found at `begin`.
 * @return The index where the pattern begins, or #TB_STR_NOT_FOUND.
 */
TB_API size_t tb_str_find(const char *string, size_t begin, size_t end, const char *pattern);

/**
 * @brief Find the last occurrence of a pattern that lies wholly in a range.
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param pattern Required non-null. The empty pattern is found at the end of
 * the range.
 * @return The index where the pattern begins, or #TB_STR_NOT_FOUND.
 */
TB_API size_t tb_str_rfind(const char *string, size_t begin, size_t end, const char *pattern);

/**
 * @brief Measure the run of bytes from a set at the start of a range.
 *
 * `begin + tb_str_span(...)` is the first position whose byte is not in the
 * set: this skips spaces, digits, or any other class of bytes.
 *
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param set The bytes that continue the run, as a string; required non-null.
 * @return The number of bytes in the run; 0 when the first byte is not in the
 * set or the range is empty.
 */
TB_API size_t tb_str_span(const char *string, size_t begin, size_t end, const char *set);

/**
 * @brief Measure the run of bytes from a set at the end of a range.
 * @param string Required non-null.
 * @param begin Start of the range.
 * @param end End of the range, or #TB_STR_END.
 * @param set The bytes that continue the run, as a string; required non-null.
 * @return The number of bytes in the run, counted back from the end of the
 * range.
 */
TB_API size_t tb_str_rspan(const char *string, size_t begin, size_t end, const char *set);

/* -------------------------------------------------------------------------
 * Building new strings. Each returns a NUL-terminated string from the
 * allocator, never NULL; exhaustion, or a length that overflows `size_t`,
 * calls the memory failure handler and ends the process.
 * ------------------------------------------------------------------------- */

/**
 * @brief Copy a string.
 * @param allocator Where the copy comes from, or NULL for the engine's heap.
 * @param string Required non-null.
 */
TB_API char *tb_str_dup(const tb_allocator *allocator, const char *string);

/**
 * @brief Copy a part of a string.
 * @param allocator Where the copy comes from, or NULL for the heap.
 * @param string Required non-null.
 * @param begin Start of the part; at most `end`.
 * @param end End of the part; at most the length, or #TB_STR_END.
 */
TB_API char *tb_str_sub(const tb_allocator *allocator, const char *string, size_t begin,
                        size_t end);

/**
 * @brief Join two strings end to end.
 * @param allocator Where the result comes from, or NULL for the heap.
 * @param left Required non-null.
 * @param right Required non-null.
 */
TB_API char *tb_str_cat(const tb_allocator *allocator, const char *left, const char *right);

/**
 * @brief Join any number of strings, with a separator between each two.
 * @param allocator Where the result comes from, or NULL for the heap.
 * @param strings `count` strings, each required non-null; may be NULL only
 * when `count` is 0.
 * @param count Number of strings; 0 gives the empty string.
 * @param separator Placed between neighbors, not before the first or after
 * the last; NULL for none.
 */
TB_API char *tb_str_join(const tb_allocator *allocator, const char *const *strings, size_t count,
                         const char *separator);

/**
 * @brief Make a string of several copies of another, end to end.
 * @param allocator Where the result comes from, or NULL for the heap.
 * @param string Required non-null.
 * @param count Number of copies; 0 gives the empty string.
 */
TB_API char *tb_str_repeat(const tb_allocator *allocator, const char *string, size_t count);

/**
 * @brief Make a string with the bytes of another in reverse order.
 *
 * Bytes, not characters: use it on ASCII. Reversing the bytes of multi-byte
 * UTF-8 characters produces invalid UTF-8.
 *
 * @param allocator Where the result comes from, or NULL for the heap.
 * @param string Required non-null.
 */
TB_API char *tb_str_reverse(const tb_allocator *allocator, const char *string);

/**
 * @brief Make a string with each byte of a set replaced by its counterpart.
 *
 * A byte that equals `from[i]` becomes `to[i]`; other bytes are copied. For
 * example, mapping `"\\"` to `"/"` turns backslashes into slashes, and mapping
 * the capital letters to the small ones lowers the case of ASCII text.
 *
 * @param allocator Where the result comes from, or NULL for the heap.
 * @param string Required non-null.
 * @param from The bytes to replace; required non-null. When a byte occurs
 * twice, its last counterpart is used.
 * @param to Their counterparts; required non-null, with the length of `from`.
 */
TB_API char *tb_str_map(const tb_allocator *allocator, const char *string, const char *from,
                        const char *to);

/**
 * @brief Release a string built by one of the functions above.
 * @param allocator The allocator the string came from, or NULL for the heap.
 * @param string String to release; NULL is ignored. Its length must be what
 * it was when it was built: a string shortened by writing a terminator into
 * it must be released with tb_allocator_free() and its original size.
 */
TB_API void tb_str_free(const tb_allocator *allocator, char *string);

#endif
