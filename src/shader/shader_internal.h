/* The shader compiler's internal interfaces: tokens, the lexer, virtual
 * paths, and the preprocessed token stream the parser reads. */
#ifndef TB_SHADER_INTERNAL_H
#define TB_SHADER_INTERNAL_H

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/shader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a token is. */
typedef enum tb_shader_token_kind
{
    TB_SHADER_TOKEN_IDENTIFIER, /* A name or a keyword. */
    TB_SHADER_TOKEN_NUMBER,     /* A preprocessing number: `1`, `0x1F`, `1.5e-3f`, `2.0h`. */
    TB_SHADER_TOKEN_STRING,     /* "text", quotes included. */
    TB_SHADER_TOKEN_CHARACTER,  /* 'c', quotes included. */
    TB_SHADER_TOKEN_PUNCTUATOR, /* An operator or a separator. */
    TB_SHADER_TOKEN_OTHER,      /* A byte that is none of those, which the parser refuses. */
    TB_SHADER_TOKEN_PRAGMA,     /* A pragma passed on; its text is what follows `#pragma`. */
    TB_SHADER_TOKEN_END         /* The end of a file; never in the preprocessed stream. */
} tb_shader_token_kind;

/* The token is the first of its line. */
#define TB_SHADER_TOKEN_LINE_START 1u
/* Space or a comment comes before the token. */
#define TB_SHADER_TOKEN_SPACE_BEFORE 2u
/* A string or character constant that has no closing quote on its line. It
 * is an error only where the preprocessor reads it, not in a skipped group. */
#define TB_SHADER_TOKEN_UNTERMINATED 4u

/* Where a token is: an index into the compile's files, a line and a column, from 1. */
typedef struct tb_shader_location
{
    uint32_t file;
    uint32_t line;
    uint32_t column;
} tb_shader_location;

typedef struct tb_shader_token
{
    tb_shader_token_kind kind;
    unsigned flags;
    const char *text; /* Its spelling, line splices removed; not NUL-terminated. */
    size_t length;
    tb_shader_location location;
} tb_shader_token;

/* Receives an error of the lexer or of the condition evaluator. */
typedef void tb_shader_error_function(void *context, tb_shader_location location,
                                      const char *message);

/* Split a file's text into tokens, in `arena`, ending with an END token.
 * The only error it reports is an unterminated comment. Comments become
 * TB_SHADER_TOKEN_SPACE_BEFORE, and a line splice (a `\` before the end of a
 * line) joins two lines. A file that starts with a UTF-8 byte order mark has
 * it skipped. Returns the token count, END included. */
size_t tb_shader_lex(const char *text, size_t length, uint32_t file, tb_arena *arena,
                     tb_shader_error_function *error, void *context, tb_shader_token **tokens);

/* The length of the punctuator at the start of `text`, or 0 if none starts there. */
size_t tb_shader_punctuator_length(const char *text, size_t length);

/* Whether a token is the punctuator `spelling`. */
bool tb_shader_token_is(const tb_shader_token *token, const char *spelling);

/* Resolve an include's text against the file that includes it: a path that
 * starts with `/` stands as it is, any other is relative to the including
 * file's directory, and `.` and `..` components are applied. Returns the
 * resolved path in `arena`, NUL-terminated, or NULL when it climbs above the
 * root or is not a valid virtual path. */
char *tb_shader_path_resolve(const char *including, const char *include, size_t include_length,
                             tb_arena *arena, size_t *length);

/* Evaluate the expression of `#if` or `#elif`, whose macros are expanded and
 * whose `defined` operators are replaced: an identifier left is 0, except
 * `true`, which is 1. Arithmetic is on signed 64-bit integers, and overflow,
 * division by zero and a shift out of range are errors, except in operands
 * that `&&`, `||` and `?:` do not evaluate. Returns false after an error. */
bool tb_shader_evaluate_condition(const tb_shader_token *tokens, size_t count,
                                  tb_shader_location directive, tb_shader_error_function *error,
                                  void *context, int64_t *value);

/* The preprocessed tokens of a shader: what the parser reads. */
typedef struct tb_shader_unit
{
    tb_shader_token *tokens; /* In the order read; no END token. */
    size_t token_count;
    const char *const *files; /* Virtual paths, indexed by tb_shader_location.file. */
    const bool *files_read;   /* Which files were read: the dependencies. */
    size_t file_count;
    tb_shader_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t error_count;
} tb_shader_unit;

/* Preprocess a request into tokens, all in `arena`. Returns OK, INVALID or
 * FAILED; the unit holds its diagnostics in every case. Running out of memory
 * ends the process. */
tb_shader_result tb_shader_preprocess_unit(const tb_shader_preprocess_request *request,
                                           tb_arena *arena, tb_shader_unit *unit);

/* Lay out a unit's tokens as text, appended to `text` (of char), each on the
 * line it came from where that can be kept, as tb_shader_preprocess() writes
 * it; with `line_markers`, a jump to another file or line is a `#line`.
 * `offsets` (of size_t), when not NULL, receives where each token starts in
 * the text, in the unit's order, which is the text's. */
void tb_shader_render(const tb_shader_unit *unit, bool line_markers, tb_array *text,
                      tb_array *offsets);

#endif
