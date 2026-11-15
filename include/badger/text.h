/** @file text.h
 * @brief Text: a string as a length and a pointer to bytes that do not change.
 *
 * A tb_text is a small value, passed and returned like an integer. It does
 * not own its bytes and needs no terminator, so a part of a text is another
 * text that points into the same bytes: taking a substring copies nothing and
 * allocates nothing. A text may contain any byte, a zero byte included.
 *
 * @code
 * const tb_text path = tb_text_from_cstr("assets/textures/stone.png");
 * const size_t slash = tb_text_rfind_char(path, 0, TB_TEXT_END, '/');
 * const tb_text name = tb_text_sub(path, slash + 1, TB_TEXT_END); // "stone.png", not copied
 *
 * if (tb_text_ends_with(name, TB_TEXT(".png")))
 * ...
 * @endcode
 *
 * **Lifetimes.** A text is valid for as long as the bytes it points to. A text
 * made by tb_text_from_cstr(), tb_text_make(), tb_text_sub(), or
 * tb_text_trim() borrows bytes that someone else owns. The functions that
 * take an allocator, such as tb_text_copy() and tb_text_cat(), produce new
 * bytes, which the caller owns and releases with tb_text_free(), or allocates
 * in an arena and releases with it. The usual pattern is to take texts apart
 * freely, and to copy a text into an arena when it must outlive its source.
 *
 * **The empty text** is the zero-initialized value. It has length 0 and its
 * pointer may be NULL.
 *
 * **Positions** are byte indexes from 0, and a range runs from `begin` up to,
 * but not including, `end`, as in str.h. #TB_TEXT_END as `end` means the end
 * of the text.
 *
 * **Bytes, not characters.** Text is UTF-8 and these functions do not decode
 * it. The notes in str.h apply: searching and comparing are correct for
 * UTF-8, positions count bytes, and tb_text_reverse() and tb_text_map() are
 * for ASCII.
 *
 * @par Threading
 * Thread-safe: a text is an immutable value and no function keeps state. The
 * caller ensures the bytes are not changed or released while in use.
 */
#ifndef TB_TEXT_H
#define TB_TEXT_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A text: `length` bytes at `bytes`. Read the members freely; make a
 * text with the functions and macros below.
 */
typedef struct tb_text
{
    size_t length;     /**< Number of bytes. */
    const char *bytes; /**< The bytes; not terminated. May be NULL when `length` is 0. */
} tb_text;

/** @brief Returned by the search functions when nothing matches. */
#define TB_TEXT_NOT_FOUND SIZE_MAX

/** @brief As the `end` of a range: the end of the text. */
#define TB_TEXT_END SIZE_MAX

/**
 * @brief The text of a string literal, as an expression: `TB_TEXT(".png")`.
 * @param literal A string literal. Its terminator is not part of the text.
 */
#define TB_TEXT(literal) ((tb_text){sizeof(literal) - 1, (literal)})

/**
 * @brief The text of a string literal, as an initializer for a constant:
 * `static const tb_text extension = TB_TEXT_INIT(".png");`.
 * @param literal A string literal.
 */
#define TB_TEXT_INIT(literal) {sizeof(literal) - 1, (literal)}

/** @brief The decimal digits, for use as a set. */
#define TB_TEXT_DIGITS TB_TEXT("0123456789")
/** @brief The capital ASCII letters, for use as a set or with tb_text_map(). */
#define TB_TEXT_UPPERCASE TB_TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
/** @brief The small ASCII letters, for use as a set or with tb_text_map(). */
#define TB_TEXT_LOWERCASE TB_TEXT("abcdefghijklmnopqrstuvwxyz")
/** @brief The ASCII white space characters, for use as a set. */
#define TB_TEXT_WHITESPACE TB_TEXT(" \t\n\r\f\v")

/* -------------------------------------------------------------------------
 * Making texts that borrow their bytes. Nothing is copied or allocated.
 * ------------------------------------------------------------------------- */

/**
 * @brief The text of a C string, without its terminator.
 * @param string Required non-null. The text borrows it.
 */
TB_API tb_text tb_text_from_cstr(const char *string);

/**
 * @brief The text of `length` bytes.
 * @param bytes The bytes, which the text borrows; may be NULL only when
 * `length` is 0.
 * @param length Number of bytes.
 */
TB_API tb_text tb_text_make(const char *bytes, size_t length);

/**
 * @brief A part of a text.
 * @param text The text.
 * @param begin Start of the part; at most `end`.
 * @param end End of the part; at most the length, or #TB_TEXT_END.
 * @return A text that points into the bytes of `text`. An empty part points
 * to where it would begin, so its position in the text is not lost.
 */
TB_API tb_text tb_text_sub(tb_text text, size_t begin, size_t end);

/**
 * @brief A text without the bytes of a set at its start and at its end.
 * @param text The text.
 * @param set The bytes to drop, such as #TB_TEXT_WHITESPACE.
 * @return A text that points into the bytes of `text`.
 */
TB_API tb_text tb_text_trim(tb_text text, tb_text set);

/* -------------------------------------------------------------------------
 * Comparing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Compare two texts byte by byte.
 * @return Negative, zero, or positive as `left` sorts before, equal to, or
 * after `right`. Bytes compare as unsigned values; a text that is a prefix of
 * the other sorts first.
 */
TB_API int tb_text_compare(tb_text left, tb_text right);

/** @brief Whether two texts have the same length and bytes. */
TB_API bool tb_text_equal(tb_text left, tb_text right);

/** @brief Whether a text begins with another. Every text begins with the empty text. */
TB_API bool tb_text_starts_with(tb_text text, tb_text prefix);

/** @brief Whether a text ends with another. Every text ends with the empty text. */
TB_API bool tb_text_ends_with(tb_text text, tb_text suffix);

/**
 * @brief Whether a pattern occurs in a text at a position.
 * @param text The text.
 * @param position Where the pattern must begin; at most the length.
 * @param pattern The pattern. The empty pattern occurs at every position.
 */
TB_API bool tb_text_matches_at(tb_text text, size_t position, tb_text pattern);

/* -------------------------------------------------------------------------
 * Searching a range of a text. `begin` is at most `end`, and `end` is at
 * most the length of the text or is TB_TEXT_END. A search returns an index
 * into the text, or TB_TEXT_NOT_FOUND.
 * ------------------------------------------------------------------------- */

/** @brief Find the first occurrence of a byte in a range. */
TB_API size_t tb_text_find_char(tb_text text, size_t begin, size_t end, char character);

/** @brief Find the last occurrence of a byte in a range. */
TB_API size_t tb_text_rfind_char(tb_text text, size_t begin, size_t end, char character);

/** @brief Find the first byte in a range that is one of the bytes of `set`. */
TB_API size_t tb_text_find_any(tb_text text, size_t begin, size_t end, tb_text set);

/** @brief Find the last byte in a range that is one of the bytes of `set`. */
TB_API size_t tb_text_rfind_any(tb_text text, size_t begin, size_t end, tb_text set);

/**
 * @brief Find the first occurrence of a pattern that lies wholly in a range.
 * The empty pattern is found at `begin`.
 */
TB_API size_t tb_text_find(tb_text text, size_t begin, size_t end, tb_text pattern);

/**
 * @brief Find the last occurrence of a pattern that lies wholly in a range.
 * The empty pattern is found at the end of the range.
 */
TB_API size_t tb_text_rfind(tb_text text, size_t begin, size_t end, tb_text pattern);

/**
 * @brief Measure the run of bytes from a set at the start of a range.
 * @return The number of bytes in the run.
 */
TB_API size_t tb_text_span(tb_text text, size_t begin, size_t end, tb_text set);

/**
 * @brief Measure the run of bytes from a set at the end of a range.
 * @return The number of bytes in the run, counted back from the end.
 */
TB_API size_t tb_text_rspan(tb_text text, size_t begin, size_t end, tb_text set);

/* -------------------------------------------------------------------------
 * Making texts with bytes of their own. Each takes its bytes from the
 * allocator, NULL for the engine's heap, and never fails: exhaustion, or a
 * length that overflows `size_t`, calls the memory failure handler and ends
 * the process. An empty result allocates nothing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Copy the bytes of a text, so that the copy outlives the original.
 * @param allocator Where the bytes come from, or NULL for the heap.
 * @param text The text to copy.
 */
TB_API tb_text tb_text_copy(const tb_allocator *allocator, tb_text text);

/**
 * @brief Copy a text into a new C string.
 * @param allocator Where the string comes from, or NULL for the heap.
 * @param text The text. If it contains a zero byte, the C string appears to
 * end there.
 * @return The bytes of the text and a terminator; never NULL. Release it with
 * tb_allocator_free() and a size of the length of the text plus one.
 */
TB_API char *tb_text_to_cstr(const tb_allocator *allocator, tb_text text);

/** @brief Join two texts end to end. */
TB_API tb_text tb_text_cat(const tb_allocator *allocator, tb_text left, tb_text right);

/**
 * @brief Join any number of texts, with a separator between each two.
 * @param allocator Where the bytes come from, or NULL for the heap.
 * @param texts `count` texts; may be NULL only when `count` is 0.
 * @param count Number of texts.
 * @param separator Placed between neighbors; the empty text for none.
 */
TB_API tb_text tb_text_join(const tb_allocator *allocator, const tb_text *texts, size_t count,
                            tb_text separator);

/** @brief Make a text of `count` copies of another, end to end. */
TB_API tb_text tb_text_repeat(const tb_allocator *allocator, tb_text text, size_t count);

/** @brief Make a text with the bytes of another in reverse order. For ASCII. */
TB_API tb_text tb_text_reverse(const tb_allocator *allocator, tb_text text);

/**
 * @brief Make a text with each byte of a set replaced by its counterpart.
 *
 * A byte that equals `from.bytes[i]` becomes `to.bytes[i]`; other bytes are
 * copied. Mapping #TB_TEXT_UPPERCASE to #TB_TEXT_LOWERCASE lowers the case of
 * ASCII text.
 *
 * @param allocator Where the bytes come from, or NULL for the heap.
 * @param text The text.
 * @param from The bytes to replace. When a byte occurs twice, its last
 * counterpart is used.
 * @param to Their counterparts; a text with the length of `from`.
 */
TB_API tb_text tb_text_map(const tb_allocator *allocator, tb_text text, tb_text from, tb_text to);

/**
 * @brief Release the bytes of a text made by one of the functions above.
 * @param allocator The allocator the bytes came from, or NULL for the heap.
 * @param text The text, with the length it was returned with. The empty text
 * is ignored. A part of a text, from tb_text_sub(), must not be released.
 */
TB_API void tb_text_free(const tb_allocator *allocator, tb_text text);

/* -------------------------------------------------------------------------
 * Formatting.
 * ------------------------------------------------------------------------- */

/**
 * @brief Register the `%T` conversion with the formatter (fmt.h).
 *
 * Afterwards a tb_text, passed by value, can be formatted like a string: the
 * width pads it and the precision cuts it.
 *
 * @code
 * tb_text_register_format();
 * tb_fmt_print("loading %T (%zu bytes)\n", name, name.length);
 * @endcode
 *
 * @par Threading Stage-owned, like tb_fmt_register(): call during
 * single-threaded startup. Calling it again changes nothing.
 */
TB_API void tb_text_register_format(void);

#endif
