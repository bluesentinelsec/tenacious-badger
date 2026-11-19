/** @file json.h
 * @brief JSON (RFC 8259): a reader that reports values as it parses, a
 * document tree built from it in an arena, and a writer.
 *
 * Configuration, asset metadata, glTF, and tool output are JSON. Two ways in,
 * and one way out:
 *
 * - **A reader** walks a text and reports each value as an event: a number,
 *   a string, the start or end of an array or object. It allocates once, a
 *   stack as deep as the nesting allowed, and never copies the text.
 * - **A document** (tb_json_parse()) is the whole text as a tree of values in
 *   an arena of the caller's, with strings decoded and object members
 *   indexed for lookup. The tree does not borrow the text.
 * - **A writer** writes values to a stream, compact or indented, and can
 *   write a whole document. Whatever it writes, the reader reads back.
 *
 * **The text is untrusted.** Parsing never recurses, so no text can exhaust
 * the stack; nesting deeper than the limit and texts longer than the limit
 * are errors, as are invalid UTF-8, unpaired surrogates, control characters
 * in strings, and every form RFC 8259 does not allow. An error reports the
 * byte offset, line, and column where it was found, and why.
 *
 * **Numbers keep their text.** An integer that fits 64 bits reads back
 * exactly, and any number reads as the nearest double; the text itself is
 * there for a caller that wants neither.
 *
 * @par Threading
 * A reader and a writer are externally synchronized. A document is immutable once parsed:
 * its accessors are thread-safe, for as long as its arena is neither reset
 * nor destroyed. tb_json_parse() is as thread-safe as the arena it is given.
 */
#ifndef TB_JSON_H
#define TB_JSON_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/arena.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Why reading failed. */
typedef enum tb_json_result
{
    TB_JSON_OK,            /**< Success. */
    TB_JSON_INVALID,       /**< An argument is malformed, such as an unknown flag. */
    TB_JSON_NO_MEMORY,     /**< The arena could not hold the document. */
    TB_JSON_TOO_LARGE,     /**< The text is longer than the limit. */
    TB_JSON_MALFORMED,     /**< The text is not JSON; the error says where and why. */
    TB_JSON_TOO_DEEP,      /**< Arrays and objects nest deeper than the limit. */
    TB_JSON_DUPLICATE_KEY, /**< An object has a key twice, which is refused unless allowed. */
    TB_JSON_OUTPUT_FAILED  /**< A writer's stream refused what it was given. */
} tb_json_result;

/** @brief Relaxations of RFC 8259, for files people edit. All are off by default. */
typedef enum tb_json_flags
{
    TB_JSON_ALLOW_COMMENTS = 1,        /**< `//` to the end of a line, and `/ * ... * /`. */
    TB_JSON_ALLOW_TRAILING_COMMAS = 2, /**< A comma before `]` or `}`. */
    TB_JSON_ALLOW_DUPLICATE_KEYS = 4   /**< Keep every member; a lookup finds the last. */
} tb_json_flags;

/** @brief Nesting allowed when the options give none. */
#define TB_JSON_DEFAULT_MAX_DEPTH ((size_t)256)

/** @brief Text allowed when the options give no limit: 256 MiB. */
#define TB_JSON_DEFAULT_MAX_LENGTH ((size_t)256 << 20)

/** @brief How to read. Zero-initialize for strict JSON and the default limits. */
typedef struct tb_json_options
{
    unsigned flags;    /**< A combination of tb_json_flags. */
    size_t max_depth;  /**< Arrays and objects open at once; 0 for the default. */
    size_t max_length; /**< Bytes of text; 0 for the default. */
} tb_json_options;

/** @brief Where reading failed, and why. */
typedef struct tb_json_error
{
    tb_json_result result; /**< What failed; OK when nothing has. */
    size_t offset;         /**< Byte offset in the text. */
    size_t line;           /**< Line, from 1. */
    size_t column;         /**< Byte in the line, from 1. */
    const char *reason;    /**< A short phrase in lower case; static; "" when nothing failed. */
} tb_json_error;

/* -------------------------------------------------------------------------
 * The reader.
 * ------------------------------------------------------------------------- */

/** @brief What the reader found next. */
typedef enum tb_json_event
{
    TB_JSON_EVENT_NULL,         /**< `null`. */
    TB_JSON_EVENT_FALSE,        /**< `false`. */
    TB_JSON_EVENT_TRUE,         /**< `true`. */
    TB_JSON_EVENT_NUMBER,       /**< A number; its text is in the token. */
    TB_JSON_EVENT_STRING,       /**< A string value; its text, as written, is in the token. */
    TB_JSON_EVENT_KEY,          /**< The key of an object member, before its value. */
    TB_JSON_EVENT_BEGIN_ARRAY,  /**< `[`. */
    TB_JSON_EVENT_END_ARRAY,    /**< `]`. */
    TB_JSON_EVENT_BEGIN_OBJECT, /**< `{`. */
    TB_JSON_EVENT_END_OBJECT,   /**< `}`. */
    TB_JSON_EVENT_END           /**< The document is complete; every later call says so again. */
} tb_json_event;

/** @brief One event and where its text is. */
typedef struct tb_json_token
{
    tb_json_event event; /**< What was found. */
    /** For a number, its text; for a string or a key, the text between the
     * quotes as written, escapes and all. Points into the reader's text. */
    const char *text;
    size_t length; /**< Bytes of `text`. */
    size_t offset; /**< Byte offset of the event in the text. */
    bool escaped;  /**< A string or key holds escapes; decode it with tb_json_unescape(). */
} tb_json_token;

/** @brief A reader of one text. Opaque. */
typedef struct tb_json_reader tb_json_reader;

/**
 * @brief Create a reader of a text.
 * @param text Borrowed; must stay valid and unchanged while the reader is used.
 * NULL only at length zero.
 * @param length Bytes of text. The text need not end with a NUL, and a NUL in it is malformed.
 * @param options How to read, or NULL for strict JSON and the default limits.
 * @param allocator Where its stack comes from, or NULL for the heap.
 * @return The reader; never NULL. Release it with tb_json_reader_destroy().
 * @par Errors Running out of memory ends the process. Bad options, and a text
 * over the length limit, are reported by the first tb_json_reader_next().
 */
TB_API tb_json_reader *tb_json_reader_create(const char *text, size_t length,
                                             const tb_json_options *options,
                                             const tb_allocator *allocator);

/** @brief Release a reader. @param reader A reader, or NULL to do nothing. */
TB_API void tb_json_reader_destroy(tb_json_reader *reader);

/**
 * @brief Read the next event.
 * @param reader Required.
 * @param[out] token Required; the event on success, unchanged on failure.
 * @return OK, or why the text is not JSON. A failure is kept: every later call
 * returns it. tb_json_reader_error() says where.
 */
TB_API tb_json_result tb_json_reader_next(tb_json_reader *reader, tb_json_token *token);

/**
 * @brief Where and why the reader failed.
 * @param reader Required.
 * @param[out] error Required; result OK and reason "" when nothing has failed.
 */
TB_API void tb_json_reader_error(const tb_json_reader *reader, tb_json_error *error);

/**
 * @brief Decode the escapes of a string or a key from the reader, to UTF-8.
 * @param text A token's text, as the reader gave it; any other text is a
 * contract violation.
 * @param length Bytes of `text`.
 * @param output Room for the result, at least `length` bytes: decoding never lengthens.
 * @return Bytes written. A `\u0000` escape writes a zero byte, so use the length.
 */
TB_API size_t tb_json_unescape(const char *text, size_t length, char *output);

/**
 * @brief Read a number's text as a signed 64-bit integer, exactly.
 * @param text A number's text from the reader. @param length Its bytes.
 * @param[out] value Required; unchanged on failure.
 * @return false when the number has a fraction or an exponent, or does not fit.
 */
TB_API bool tb_json_number_int64(const char *text, size_t length, int64_t *value);

/**
 * @brief Read a number's text as an unsigned 64-bit integer, exactly.
 * @param text A number's text from the reader. @param length Its bytes.
 * @param[out] value Required; unchanged on failure.
 * @return false when the number is negative, has a fraction or an exponent, or does not fit.
 */
TB_API bool tb_json_number_uint64(const char *text, size_t length, uint64_t *value);

/**
 * @brief Read a number's text as the nearest double.
 * @param text A number's text from the reader. @param length Its bytes.
 * @param[out] value Required; unchanged on failure.
 * @return false when the number is too large for a double. Too small is zero.
 */
TB_API bool tb_json_number_double(const char *text, size_t length, double *value);

/* -------------------------------------------------------------------------
 * Documents.
 * ------------------------------------------------------------------------- */

/** @brief What a value is. */
typedef enum tb_json_type
{
    TB_JSON_NULL,   /**< `null`. */
    TB_JSON_BOOL,   /**< `true` or `false`. */
    TB_JSON_NUMBER, /**< A number. */
    TB_JSON_STRING, /**< A string. */
    TB_JSON_ARRAY,  /**< An array. */
    TB_JSON_OBJECT  /**< An object. */
} tb_json_type;

/** @brief A value of a document. Opaque; it lives in the document's arena. */
typedef struct tb_json_value tb_json_value;

/**
 * @brief Parse a whole text into a document.
 * @param text The text; NULL only at length zero. Not kept: the document copies what it needs.
 * @param length Bytes of text.
 * @param options How to read, or NULL for strict JSON and the default limits.
 * @param arena Where the document lives; it lasts until the arena is reset or destroyed.
 * @param[out] root Required; the document's top value on success, NULL on failure.
 * @param[out] error Optional; where and why parsing failed.
 * @return OK, or why not. On failure the arena may hold part of a document,
 * which a rewind to a mark taken before the call releases.
 * @par Errors An arena that cannot grow is NO_MEMORY. The work space for
 * open containers comes from the heap, and running out of it ends the process.
 */
TB_API tb_json_result tb_json_parse(const char *text, size_t length, const tb_json_options *options,
                                    tb_arena *arena, const tb_json_value **root,
                                    tb_json_error *error);

/** @brief What a value is. @param value Required. */
TB_API tb_json_type tb_json_type_of(const tb_json_value *value);

/**
 * @brief A boolean's value. @param value Required.
 * @param[out] out Required; unchanged on failure. @return false for any other type.
 */
TB_API bool tb_json_as_bool(const tb_json_value *value, bool *out);

/**
 * @brief A number as a signed 64-bit integer, exactly; see tb_json_number_int64().
 * @param value Required. @param[out] out Required; unchanged on failure.
 * @return false for any other type, or a number that is not such an integer.
 */
TB_API bool tb_json_as_int64(const tb_json_value *value, int64_t *out);

/**
 * @brief A number as an unsigned 64-bit integer, exactly; see tb_json_number_uint64().
 * @param value Required. @param[out] out Required; unchanged on failure.
 * @return false for any other type, or a number that is not such an integer.
 */
TB_API bool tb_json_as_uint64(const tb_json_value *value, uint64_t *out);

/**
 * @brief A number as the nearest double; see tb_json_number_double().
 * @param value Required. @param[out] out Required; unchanged on failure.
 * @return false for any other type, or a number too large for a double.
 */
TB_API bool tb_json_as_double(const tb_json_value *value, double *out);

/**
 * @brief A number's text as written, or a string decoded to UTF-8 with a NUL after it.
 * @param value Required.
 * @param[out] length Required; bytes before the NUL. A string may hold zero bytes of its own.
 * @return The text, valid as long as the document; NULL for any other type.
 */
TB_API const char *tb_json_as_text(const tb_json_value *value, size_t *length);

/**
 * @brief Values in an array, or members in an object.
 * @param value Required. @return The count; 0 for any other type.
 */
TB_API size_t tb_json_count(const tb_json_value *value);

/**
 * @brief A value of an array, by position.
 * @param value Required array; any other type is a contract violation.
 * @param index Below tb_json_count(); anything else is a contract violation.
 * @return The value; never NULL.
 */
TB_API const tb_json_value *tb_json_at(const tb_json_value *value, size_t index);

/**
 * @brief A member of an object, by position in the text.
 * @param value Required object; any other type is a contract violation.
 * @param index Below tb_json_count(); anything else is a contract violation.
 * @param[out] key Required; the decoded key, with a NUL after it.
 * @param[out] key_length Required; bytes of the key before the NUL.
 * @return The member's value; never NULL.
 */
TB_API const tb_json_value *tb_json_member_at(const tb_json_value *value, size_t index,
                                              const char **key, size_t *key_length);

/**
 * @brief Find an object's member by key, in time logarithmic in its members.
 * @param value Required.
 * @param key The decoded key; NULL only at length zero.
 * @param key_length Bytes of the key.
 * @return The value, the last one when duplicates were allowed; NULL when
 * there is none, or `value` is not an object.
 */
TB_API const tb_json_value *tb_json_find(const tb_json_value *value, const char *key,
                                         size_t key_length);

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/** @brief How a writer lays out its text. */
typedef enum tb_json_style
{
    TB_JSON_COMPACT, /**< No space at all: the fewest bytes. */
    TB_JSON_INDENTED /**< A value per line, indented two spaces a level, for people to read. */
} tb_json_style;

/** @brief A writer of one document to a stream. Opaque. */
typedef struct tb_json_writer tb_json_writer;

/**
 * @brief Create a writer of one document.
 *
 * The writer holds the text in a buffer of its own and passes it to the sink
 * when the buffer fills and at tb_json_writer_finish().
 *
 * Calls must make one well-formed document: a key before each value of an
 * object and nowhere else, ends that match their beginnings, and one value at
 * the top. Breaking that is a contract violation, as is any call after a
 * finish; a value that cannot be written as JSON is a result.
 * @param sink Required open stream that can write; borrowed, and must stay
 * open until the writer is destroyed.
 * @param style How to lay out the text.
 * @param allocator Where the writer's state comes from, or NULL for the heap.
 * @return The writer; never NULL. Release it with tb_json_writer_destroy().
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_json_writer *tb_json_writer_create(tb_stream *sink, tb_json_style style,
                                             const tb_allocator *allocator);

/** @brief Release a writer, without writing what it holds. @param writer A writer, or NULL. */
TB_API void tb_json_writer_destroy(tb_json_writer *writer);

/** @brief Write `null`. @param writer Required. @return OK, or a failure kept from before. */
TB_API tb_json_result tb_json_write_null(tb_json_writer *writer);

/**
 * @brief Write `true` or `false`. @param writer Required. @param value The value.
 * @return OK, or a failure kept from before.
 */
TB_API tb_json_result tb_json_write_bool(tb_json_writer *writer, bool value);

/**
 * @brief Write an integer, exactly. @param writer Required. @param value The value.
 * @return OK, or a failure kept from before.
 */
TB_API tb_json_result tb_json_write_int64(tb_json_writer *writer, int64_t value);

/**
 * @brief Write an unsigned integer, exactly. @param writer Required. @param value The value.
 * @return OK, or a failure kept from before.
 */
TB_API tb_json_result tb_json_write_uint64(tb_json_writer *writer, uint64_t value);

/**
 * @brief Write a double as the shortest text that reads back as the same double.
 * @param writer Required. @param value The value.
 * @return OK; INVALID for infinity or NaN, which JSON cannot hold.
 */
TB_API tb_json_result tb_json_write_double(tb_json_writer *writer, double value);

/**
 * @brief Write a number's text as it is, after checking it is a JSON number.
 * @param writer Required. @param text The text; NULL only at length zero. @param length Its bytes.
 * @return OK; INVALID for text that is not a JSON number.
 */
TB_API tb_json_result tb_json_write_number_text(tb_json_writer *writer, const char *text,
                                                size_t length);

/**
 * @brief Write a string, escaping what JSON requires: `"`, `\`, and control characters.
 * @param writer Required. @param text UTF-8; NULL only at length zero. @param length Its bytes.
 * @return OK; INVALID for text that is not UTF-8.
 */
TB_API tb_json_result tb_json_write_string(tb_json_writer *writer, const char *text, size_t length);

/**
 * @brief Write the key of the next member of an object.
 * @param writer Required; inside an object, before a value.
 * @param text UTF-8; NULL only at length zero. @param length Its bytes.
 * @return OK; INVALID for text that is not UTF-8.
 */
TB_API tb_json_result tb_json_write_key(tb_json_writer *writer, const char *text, size_t length);

/** @brief Begin an array. @param writer Required. @return OK, or a failure kept from before. */
TB_API tb_json_result tb_json_write_begin_array(tb_json_writer *writer);

/** @brief End the innermost array. @param writer Required. @return OK, or a failure kept. */
TB_API tb_json_result tb_json_write_end_array(tb_json_writer *writer);

/** @brief Begin an object. @param writer Required. @return OK, or a failure kept from before. */
TB_API tb_json_result tb_json_write_begin_object(tb_json_writer *writer);

/** @brief End the innermost object. @param writer Required. @return OK, or a failure kept. */
TB_API tb_json_result tb_json_write_end_object(tb_json_writer *writer);

/**
 * @brief Write a value of a document, and everything inside it, without recursion.
 * @param writer Required. @param value Required.
 * @return OK, or a failure. Numbers are written as their text.
 */
TB_API tb_json_result tb_json_write_value(tb_json_writer *writer, const tb_json_value *value);

/**
 * @brief Finish the document and pass the rest of it to the sink. In the
 * indented style the text ends with a line break.
 * @param writer Required; the document must be complete.
 * @return OK; OUTPUT_FAILED when the sink refused any of the text.
 */
TB_API tb_json_result tb_json_writer_finish(tb_json_writer *writer);

/**
 * @brief Name a result, for a log or a message: "malformed data", "too deep".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_json_result_name(tb_json_result result);

#endif
