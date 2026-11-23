/* The shader compiler's lexer: a file's text into preprocessing tokens, as
 * the C and HLSL preprocessors split it. */
#include "shader_internal.h"

#include "badger/array.h"
#include "badger/str.h"

#include <string.h>

/* Every punctuator of HLSL and its preprocessor, longest first, so the first
 * that matches is the longest. */
static const char *const punctuators[] = {
    ">>=", "<<=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&",
    "||",  "+=",  "-=",  "*=", "/=", "%=", "&=", "|=", "^=", "##", "::", "[",  "]",
    "(",   ")",   "{",   "}",  ".",  "&",  "*",  "+",  "-",  "~",  "!",  "/",  "%",
    "<",   ">",   "^",   "|",  "?",  ":",  ";",  "=",  ",",  "#",
};

static const unsigned char byte_order_mark[] = {0xEF, 0xBB, 0xBF};

size_t tb_shader_punctuator_length(const char *text, size_t length)
{
    for (size_t index = 0; index < sizeof(punctuators) / sizeof(punctuators[0]); index++)
    {
        const size_t size = tb_str_length(punctuators[index]);
        if (size <= length && memcmp(text, punctuators[index], size) == 0)
        {
            return size;
        }
    }
    return 0;
}

bool tb_shader_token_is(const tb_shader_token *token, const char *spelling)
{
    const size_t size = tb_str_length(spelling);
    return token->kind == TB_SHADER_TOKEN_PUNCTUATOR && token->length == size &&
           memcmp(token->text, spelling, size) == 0;
}

/* The text, and a position in it that steps over line splices. */
typedef struct lexer
{
    const char *text;
    size_t length;
    size_t offset; /* Of the next byte, never at a splice. */
    uint32_t file;
    uint32_t line;
    uint32_t column;
    tb_shader_error_function *error;
    void *context;
} lexer;

/* The length of the line break at `offset`, or 0 if there is none. */
static size_t line_break_length(const lexer *state, size_t offset)
{
    if (offset >= state->length)
    {
        return 0;
    }
    if (state->text[offset] == '\n')
    {
        return 1;
    }
    if (state->text[offset] == '\r')
    {
        const bool crlf = offset + 1 < state->length && state->text[offset + 1] == '\n';
        return crlf ? 2 : 1;
    }
    return 0;
}

/* Step over any line splices at the position. */
static void skip_splices(lexer *state)
{
    while (state->offset < state->length && state->text[state->offset] == '\\')
    {
        const size_t size = line_break_length(state, state->offset + 1);
        if (size == 0)
        {
            return;
        }
        state->offset += 1 + size;
        state->line++;
        state->column = 1;
    }
}

static bool at_end(const lexer *state)
{
    return state->offset >= state->length;
}

/* The next byte, or 0 at the end. */
static char peek(const lexer *state)
{
    return at_end(state) ? '\0' : state->text[state->offset];
}

/* The byte after the next, splices skipped, or 0 at the end. */
static char peek_second(const lexer *state)
{
    lexer copy = *state;
    copy.offset++;
    skip_splices(&copy);
    return peek(&copy);
}

/* Move past the next byte; a line break counts as one. */
static void advance(lexer *state)
{
    const size_t size = line_break_length(state, state->offset);
    if (size > 0)
    {
        state->offset += size;
        state->line++;
        state->column = 1;
    }
    else
    {
        state->offset++;
        state->column++;
    }
    skip_splices(state);
}

static bool is_identifier_start(char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

static bool is_digit(char byte)
{
    return byte >= '0' && byte <= '9';
}

static bool is_identifier_byte(char byte)
{
    return is_identifier_start(byte) || is_digit(byte);
}

static bool is_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f';
}

static bool is_line_break(char byte)
{
    return byte == '\n' || byte == '\r';
}

static tb_shader_location location_of(const lexer *state)
{
    const tb_shader_location location = {state->file, state->line, state->column};
    return location;
}

/* Skip space and comments. Sets *line_start when a line break was crossed
 * outside a block comment, and *space when anything was skipped. */
static void skip_space(lexer *state, bool *line_start, bool *space)
{
    while (!at_end(state))
    {
        const char byte = peek(state);
        if (is_space(byte))
        {
            *space = true;
            advance(state);
        }
        else if (is_line_break(byte))
        {
            *line_start = true;
            *space = true;
            advance(state);
        }
        else if (byte == '/' && peek_second(state) == '/')
        {
            *space = true;
            while (!at_end(state) && !is_line_break(peek(state)))
            {
                advance(state);
            }
        }
        else if (byte == '/' && peek_second(state) == '*')
        {
            const tb_shader_location start = location_of(state);
            *space = true;
            advance(state);
            advance(state);
            while (!at_end(state) && !(peek(state) == '*' && peek_second(state) == '/'))
            {
                advance(state);
            }
            if (at_end(state))
            {
                state->error(state->context, start, "unterminated comment");
                return;
            }
            advance(state);
            advance(state);
        }
        else
        {
            return;
        }
    }
}

/* A preprocessing number: a digit, or `.` and a digit, then digits, letters,
 * `_`, `.`, and a sign after an exponent letter. */
static void scan_number(lexer *state)
{
    char previous = '\0';
    while (!at_end(state))
    {
        const char byte = peek(state);
        const bool exponent_sign =
            (byte == '+' || byte == '-') &&
            (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P');
        if (!is_identifier_byte(byte) && byte != '.' && !exponent_sign)
        {
            return;
        }
        previous = byte;
        advance(state);
    }
}

/* A string or a character constant, from its opening quote. Returns false
 * when it ends at a line break or the end of the text instead of its quote. */
static bool scan_quoted(lexer *state, char quote)
{
    advance(state);
    while (!at_end(state) && peek(state) != quote)
    {
        if (is_line_break(peek(state)))
        {
            return false;
        }
        if (peek(state) == '\\')
        {
            advance(state);
            if (at_end(state) || is_line_break(peek(state)))
            {
                return false;
            }
        }
        advance(state);
    }
    if (at_end(state))
    {
        return false;
    }
    advance(state);
    return true;
}

/* The spelling of the text from `begin` to the lexer's position, with line
 * splices removed: borrowed from the text where it has none. */
static const char *spelling(const lexer *state, size_t begin, tb_arena *arena, size_t *length)
{
    const size_t raw_length = state->offset - begin;
    const char *const raw = state->text + begin;
    if (memchr(raw, '\\', raw_length) == NULL)
    {
        *length = raw_length;
        return raw;
    }
    char *const copy = tb_arena_alloc(arena, raw_length + 1, 1);
    size_t written = 0;
    lexer cursor = *state;
    cursor.offset = begin;
    while (cursor.offset < state->offset)
    {
        copy[written] = cursor.text[cursor.offset];
        written++;
        cursor.offset++;
        skip_splices(&cursor);
    }
    copy[written] = '\0';
    *length = written;
    return copy;
}

/* Scan one token from the position, which is at neither space nor the end. */
static tb_shader_token_kind scan_token(lexer *state, unsigned *flags)
{
    const char byte = peek(state);
    if (is_identifier_start(byte))
    {
        while (!at_end(state) && is_identifier_byte(peek(state)))
        {
            advance(state);
        }
        return TB_SHADER_TOKEN_IDENTIFIER;
    }
    if (is_digit(byte) || (byte == '.' && is_digit(peek_second(state))))
    {
        scan_number(state);
        return TB_SHADER_TOKEN_NUMBER;
    }
    if (byte == '"' || byte == '\'')
    {
        if (!scan_quoted(state, byte))
        {
            *flags |= TB_SHADER_TOKEN_UNTERMINATED;
        }
        return byte == '"' ? TB_SHADER_TOKEN_STRING : TB_SHADER_TOKEN_CHARACTER;
    }
    /* A punctuator is matched on the bytes, splices skipped. */
    char window[3] = {0};
    size_t window_length = 0;
    lexer cursor = *state;
    while (window_length < sizeof(window) && !at_end(&cursor))
    {
        window[window_length] = peek(&cursor);
        window_length++;
        advance(&cursor);
    }
    size_t size = tb_shader_punctuator_length(window, window_length);
    if (size == 0)
    {
        advance(state);
        return TB_SHADER_TOKEN_OTHER;
    }
    while (size > 0)
    {
        advance(state);
        size--;
    }
    return TB_SHADER_TOKEN_PUNCTUATOR;
}

size_t tb_shader_lex(const char *text, size_t length, uint32_t file, tb_arena *arena,
                     tb_shader_error_function *error, void *context, tb_shader_token **tokens)
{
    lexer state = {.text = text,
                   .length = length,
                   .offset = 0,
                   .file = file,
                   .line = 1,
                   .column = 1,
                   .error = error,
                   .context = context};
    if (length >= sizeof(byte_order_mark) &&
        memcmp(text, byte_order_mark, sizeof(byte_order_mark)) == 0)
    {
        state.offset = sizeof(byte_order_mark);
    }
    skip_splices(&state);

    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_array *const list = TB_ARRAY_CREATE(&allocator, tb_shader_token);
    bool line_start = true;
    bool space = false;
    for (;;)
    {
        skip_space(&state, &line_start, &space);
        tb_shader_token token = {0};
        token.location = location_of(&state);
        token.flags = (line_start ? TB_SHADER_TOKEN_LINE_START : 0u) |
                      (space ? TB_SHADER_TOKEN_SPACE_BEFORE : 0u);
        if (at_end(&state))
        {
            token.kind = TB_SHADER_TOKEN_END;
            token.text = "";
            token.flags |= TB_SHADER_TOKEN_LINE_START;
            (void)tb_array_push(list, &token);
            break;
        }
        const size_t begin = state.offset;
        token.kind = scan_token(&state, &token.flags);
        token.text = spelling(&state, begin, arena, &token.length);
        (void)tb_array_push(list, &token);
        line_start = false;
        space = false;
    }
    *tokens = tb_array_data(list);
    return tb_array_length(list);
}
