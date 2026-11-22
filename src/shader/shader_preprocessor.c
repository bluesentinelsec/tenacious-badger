/* The shader compiler's preprocessor: includes, conditionals, and macros, as
 * DXC's preprocessor applies them.
 *
 * Tokens flow through a linked list, so a macro's expansion is put in front
 * of the tokens that follow it and read again. Each token carries the set of
 * macros it came from (its hide set, after Prosser's algorithm), and a macro
 * never expands inside its own expansion. */
#include "shader_internal.h"

#include "badger/array.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/str.h"
#include "badger/table.h"

#include <string.h>

/* Expansions nested inside the arguments of others. */
#define MAX_EXPANSION_NESTING ((size_t)256)

/* -------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */

typedef enum macro_kind
{
    MACRO_OBJECT,
    MACRO_FUNCTION,
    MACRO_LINE, /* __LINE__ */
    MACRO_FILE  /* __FILE__ */
} macro_kind;

typedef struct name_key
{
    const char *text;
    size_t length;
} name_key;

typedef struct macro
{
    name_key name;
    macro_kind kind;
    name_key *parameters;
    size_t parameter_count; /* __VA_ARGS__ included, for a variadic macro. */
    bool variadic;
    tb_shader_token *body;
    size_t body_count;
} macro;

/* The macros a token came from. */
typedef struct hideset
{
    const macro *macro;
    const struct hideset *next;
} hideset;

/* The token is the end of a file, which closes its frame. */
#define PP_FILE_END 1u
/* The token came from a macro's expansion, so it never starts a directive. */
#define PP_FROM_MACRO 2u

typedef struct pp_token
{
    tb_shader_token token;
    unsigned pp_flags;
    const hideset *hidden;
    struct pp_token *next;
} pp_token;

typedef struct source_file
{
    name_key path; /* NUL-terminated. */
    const char *text;
    size_t length;
    tb_shader_token *tokens;
    size_t token_count;
    bool read; /* Read from the request or the loader, so a dependency. */
    bool once; /* Has `#pragma once`. */
} source_file;

/* A file being read, and what `#line` says of it. */
typedef struct frame
{
    uint32_t file;
    size_t conditional_depth;
    bool renamed;      /* `#line` applies. */
    int64_t line_bias; /* Added to a line of the file. */
    uint32_t name;     /* The file it is reported as. */
} frame;

typedef struct conditional
{
    tb_shader_location location;
    bool taken;     /* A group of it has been read. */
    bool seen_else; /* `#else` has been read. */
} conditional;

typedef struct preprocessor
{
    const tb_shader_preprocess_request *request;
    tb_arena *arena;
    tb_allocator allocator;
    tb_table *macros; /* name_key to macro. */
    tb_table *paths;  /* name_key to file index + 1. */
    tb_array *files;  /* source_file */
    tb_array *conditionals;
    frame frames[TB_SHADER_MAX_INCLUDE_DEPTH];
    size_t frame_count;
    tb_array *output; /* tb_shader_token */
    tb_array *diagnostics;
    size_t error_count;
    size_t tokens_made;
    size_t max_tokens;
    tb_shader_location site; /* Of the token of a file being read, where limits are reported. */
    size_t nesting;
    bool stopped;
} preprocessor;

/* -------------------------------------------------------------------------
 * Names and diagnostics.
 * ------------------------------------------------------------------------- */

static uint64_t name_hash(const void *key)
{
    const name_key *const name = key;
    return tb_hash_bytes(name->text, name->length);
}

static bool name_equal(const void *left, const void *right)
{
    const name_key *const a = left;
    const name_key *const b = right;
    return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

static name_key token_name(const tb_shader_token *token)
{
    const name_key name = {token->text, token->length};
    return name;
}

static bool token_equals(const tb_shader_token *token, const char *text)
{
    const size_t length = tb_str_length(text);
    return token->length == length && memcmp(token->text, text, length) == 0;
}

static source_file *file_at(const preprocessor *state, uint32_t index)
{
    return tb_array_at(state->files, index);
}

static const frame *top_frame(const preprocessor *state)
{
    return &state->frames[state->frame_count - 1];
}

/* Where a location is reported, after any `#line` of the file being read. */
static tb_shader_location map_location(const preprocessor *state, tb_shader_location location)
{
    if (state->frame_count == 0)
    {
        return location;
    }
    const frame *const current = top_frame(state);
    if (!current->renamed || location.file != current->file)
    {
        return location;
    }
    int64_t line = (int64_t)location.line + current->line_bias;
    if (line < 1)
    {
        line = 1;
    }
    tb_shader_location mapped = location;
    mapped.line = (uint32_t)line;
    mapped.file = current->name;
    return mapped;
}

static void add_diagnostic(preprocessor *state, tb_shader_severity severity, const char *path,
                           size_t line, size_t column, const char *message)
{
    if (state->stopped)
    {
        return;
    }
    const tb_shader_diagnostic diagnostic = {severity, path, line, column, message};
    (void)tb_array_push(state->diagnostics, &diagnostic);
    if (severity != TB_SHADER_ERROR)
    {
        return;
    }
    state->error_count++;
    if (state->error_count == TB_SHADER_MAX_ERRORS)
    {
        const tb_shader_diagnostic last = {TB_SHADER_ERROR, path, line, column,
                                           "too many errors; stopping"};
        (void)tb_array_push(state->diagnostics, &last);
        state->stopped = true;
    }
}

static void report_at(preprocessor *state, tb_shader_severity severity, tb_shader_location location,
                      const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const message = tb_fmt_vstring(&state->allocator, format, arguments);
    va_end(arguments);
    const tb_shader_location mapped = map_location(state, location);
    add_diagnostic(state, severity, file_at(state, mapped.file)->path.text, mapped.line,
                   mapped.column, message);
}

#define ERROR_AT(state, location, ...) report_at((state), TB_SHADER_ERROR, (location), __VA_ARGS__)
#define WARNING_AT(state, location, ...)                                                           \
    report_at((state), TB_SHADER_WARNING, (location), __VA_ARGS__)

/* Receives the errors of the lexer and of the condition evaluator. */
static void report_error(void *context, tb_shader_location location, const char *message)
{
    ERROR_AT((preprocessor *)context, location, "%s", message);
}

/* An error about the request, which has no file. */
static void request_error(preprocessor *state, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const message = tb_fmt_vstring(&state->allocator, format, arguments);
    va_end(arguments);
    add_diagnostic(state, TB_SHADER_ERROR, "", 0, 0, message);
}

/* -------------------------------------------------------------------------
 * Tokens, lists, and hide sets.
 * ------------------------------------------------------------------------- */

/* A new token, or NULL once the compile has made too many. */
static pp_token *make_token(preprocessor *state, const tb_shader_token *token, unsigned pp_flags,
                            const hideset *hidden)
{
    if (state->stopped)
    {
        return NULL;
    }
    if (state->tokens_made == state->max_tokens)
    {
        ERROR_AT(state, state->site, "the shader expands to more than %zu tokens",
                 state->max_tokens);
        state->stopped = true;
        return NULL;
    }
    state->tokens_made++;
    pp_token *const made = TB_ARENA_NEW(state->arena, pp_token);
    made->token = *token;
    made->pp_flags = pp_flags;
    made->hidden = hidden;
    made->next = NULL;
    return made;
}

/* A list being built, by its first token and the link to fill next. */
typedef struct token_list
{
    pp_token *head;
    pp_token **tail;
    pp_token *last;
} token_list;

static void list_init(token_list *list)
{
    list->head = NULL;
    list->tail = &list->head;
    list->last = NULL;
}

static void list_append(token_list *list, pp_token *token)
{
    if (token == NULL)
    {
        return;
    }
    token->next = NULL;
    *list->tail = token;
    list->tail = &token->next;
    list->last = token;
}

/* End a list with an END token and return its first token. */
static pp_token *list_finish(preprocessor *state, token_list *list, tb_shader_location location)
{
    pp_token *const end = TB_ARENA_NEW(state->arena, pp_token);
    end->token.kind = TB_SHADER_TOKEN_END;
    end->token.flags = TB_SHADER_TOKEN_LINE_START;
    end->token.text = "";
    end->token.location = location;
    *list->tail = end;
    return list->head;
}

static bool hideset_contains(const hideset *set, const macro *wanted)
{
    for (const hideset *entry = set; entry != NULL; entry = entry->next)
    {
        if (entry->macro == wanted)
        {
            return true;
        }
    }
    return false;
}

static const hideset *hideset_add(preprocessor *state, const hideset *set, const macro *added)
{
    if (hideset_contains(set, added))
    {
        return set;
    }
    hideset *const entry = TB_ARENA_NEW(state->arena, hideset);
    entry->macro = added;
    entry->next = set;
    return entry;
}

static const hideset *hideset_union(preprocessor *state, const hideset *left, const hideset *right)
{
    const hideset *result = right;
    for (const hideset *entry = left; entry != NULL; entry = entry->next)
    {
        result = hideset_add(state, result, entry->macro);
    }
    return result;
}

static const hideset *hideset_intersection(preprocessor *state, const hideset *left,
                                           const hideset *right)
{
    const hideset *result = NULL;
    for (const hideset *entry = left; entry != NULL; entry = entry->next)
    {
        if (hideset_contains(right, entry->macro))
        {
            result = hideset_add(state, result, entry->macro);
        }
    }
    return result;
}

static bool is_end(const pp_token *token)
{
    return token->token.kind == TB_SHADER_TOKEN_END;
}

static bool starts_line(const pp_token *token)
{
    return (token->token.flags & TB_SHADER_TOKEN_LINE_START) != 0;
}

/* Whether a token is on the same line as the one before it, and not the end. */
static bool continues_line(const pp_token *token)
{
    return !is_end(token) && !starts_line(token);
}

/* Whether the token is `#` at the start of a line of a file. */
static bool starts_directive(const pp_token *token)
{
    return starts_line(token) && (token->pp_flags & PP_FROM_MACRO) == 0 &&
           tb_shader_token_is(&token->token, "#");
}

/* The first token of the next line. */
static pp_token *next_line(pp_token *token)
{
    while (continues_line(token))
    {
        token = token->next;
    }
    return token;
}

/* Copies of the tokens from `begin` to the end of its line, as a list that ends with END. */
static pp_token *copy_line(preprocessor *state, const pp_token *begin)
{
    token_list list = {0};
    list_init(&list);
    const pp_token *token = begin;
    while (continues_line(token))
    {
        list_append(&list, make_token(state, &token->token, token->pp_flags, token->hidden));
        token = token->next;
    }
    return list_finish(state, &list, begin->token.location);
}

/* Copies of a list's tokens, as a list that ends with END. */
static pp_token *copy_list(preprocessor *state, const pp_token *list)
{
    token_list copy = {0};
    list_init(&copy);
    const pp_token *token = list;
    while (!is_end(token))
    {
        list_append(&copy, make_token(state, &token->token, token->pp_flags, token->hidden));
        token = token->next;
    }
    return list_finish(state, &copy, token->token.location);
}

/* -------------------------------------------------------------------------
 * Expanding macros.
 * ------------------------------------------------------------------------- */

static macro *find_macro(const preprocessor *state, const tb_shader_token *token)
{
    if (token->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        return NULL;
    }
    const name_key name = token_name(token);
    return tb_table_get(state->macros, &name);
}

/* The index of the parameter a token names, or -1. */
static long find_parameter(const macro *definition, const tb_shader_token *token)
{
    if (definition->kind != MACRO_FUNCTION || token->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        return -1;
    }
    const name_key name = token_name(token);
    for (size_t index = 0; index < definition->parameter_count; index++)
    {
        if (name_equal(&definition->parameters[index], &name))
        {
            return (long)index;
        }
    }
    return -1;
}

/* One argument of an invocation: its tokens as written, and once needed, expanded. */
typedef struct argument
{
    pp_token *raw; /* Ends with END. */
    pp_token *expanded;
} argument;

static pp_token *expand_list(preprocessor *state, pp_token *list);

static const pp_token *expanded_argument(preprocessor *state, argument *arg)
{
    if (arg->expanded == NULL)
    {
        arg->expanded = expand_list(state, copy_list(state, arg->raw));
    }
    return arg->expanded;
}

/* A copied token's flags where it is placed: the first of an expansion takes
 * the space and line flags of the invocation, and no other starts a line. */
static unsigned placed_flags(unsigned own, bool first, unsigned first_flags)
{
    const unsigned layout = TB_SHADER_TOKEN_LINE_START | TB_SHADER_TOKEN_SPACE_BEFORE;
    if (first)
    {
        return (own & ~layout) | first_flags;
    }
    return own & ~TB_SHADER_TOKEN_LINE_START;
}

/* Where an expansion's tokens go, and how its first is spaced. */
typedef struct expansion
{
    token_list list;
    bool first;
    unsigned first_flags;
    tb_shader_location location; /* Of the invocation. */
} expansion;

/* Append a copy of a token of an argument, which keeps its own location. */
static void append_argument_token(preprocessor *state, expansion *out, const pp_token *token)
{
    pp_token *const copy = make_token(state, &token->token, PP_FROM_MACRO, token->hidden);
    if (copy == NULL)
    {
        return;
    }
    copy->token.flags = placed_flags(copy->token.flags, out->first, out->first_flags);
    out->first = false;
    list_append(&out->list, copy);
}

/* Append copies of a list's tokens, up to its END. */
static void append_list(preprocessor *state, expansion *out, const pp_token *source)
{
    for (const pp_token *token = source; !is_end(token); token = token->next)
    {
        append_argument_token(state, out, token);
    }
}

/* Append a copy of a token of a macro's body, placed where the macro was invoked. */
static void append_body_token(preprocessor *state, expansion *out, const tb_shader_token *token)
{
    pp_token *const copy = make_token(state, token, PP_FROM_MACRO, NULL);
    if (copy == NULL)
    {
        return;
    }
    copy->token.location = out->location;
    copy->token.flags = placed_flags(copy->token.flags, out->first, out->first_flags);
    out->first = false;
    list_append(&out->list, copy);
}

/* `#` and an argument: the argument's spelling as a string. */
static void append_string(preprocessor *state, expansion *out, const tb_shader_token *hash,
                          const pp_token *raw)
{
    size_t length = 2;
    for (const pp_token *token = raw; !is_end(token); token = token->next)
    {
        length += token->token.length * 2 + 1;
    }
    char *const text = tb_arena_alloc(state->arena, length, 1);
    size_t written = 0;
    text[written] = '"';
    written++;
    for (const pp_token *token = raw; !is_end(token); token = token->next)
    {
        if (token != raw && (token->token.flags & TB_SHADER_TOKEN_SPACE_BEFORE) != 0)
        {
            text[written] = ' ';
            written++;
        }
        const bool quoted = token->token.kind == TB_SHADER_TOKEN_STRING ||
                            token->token.kind == TB_SHADER_TOKEN_CHARACTER;
        for (size_t index = 0; index < token->token.length; index++)
        {
            const char byte = token->token.text[index];
            if (quoted && (byte == '"' || byte == '\\'))
            {
                text[written] = '\\';
                written++;
            }
            text[written] = byte;
            written++;
        }
    }
    text[written] = '"';
    written++;
    tb_shader_token string = *hash;
    string.kind = TB_SHADER_TOKEN_STRING;
    string.text = text;
    string.length = written;
    append_body_token(state, out, &string);
}

static void note_paste_error(void *context, tb_shader_location location, const char *message)
{
    (void)location;
    (void)message;
    *(bool *)context = true;
}

/* `##`: make `left` the one token spelled by it and `right`, or leave it after an error. */
static void paste(preprocessor *state, pp_token *left, const tb_shader_token *right)
{
    const size_t length = left->token.length + right->length;
    char *const text = tb_arena_alloc(state->arena, length + 1, 1);
    memcpy(text, left->token.text, left->token.length);
    memcpy(text + left->token.length, right->text, right->length);
    text[length] = '\0';
    bool failed = false;
    tb_shader_token *tokens = NULL;
    const size_t count = tb_shader_lex(text, length, left->token.location.file, state->arena,
                                       note_paste_error, &failed, &tokens);
    const bool one_token = count == 2 && tokens[0].length == length &&
                           (tokens[0].flags & TB_SHADER_TOKEN_UNTERMINATED) == 0;
    if (failed || !one_token)
    {
        ERROR_AT(state, left->token.location,
                 "pasting '%.*s' and '%.*s' does not give a valid token", (int)left->token.length,
                 left->token.text, (int)right->length, right->text);
        return;
    }
    left->token.kind = tokens[0].kind;
    left->token.text = text;
    left->token.length = length;
}

/* The right operand of `##`: paste its first token onto the expansion's last,
 * and append the rest. When the left operand gave no tokens, nothing is pasted. */
static void paste_list(preprocessor *state, expansion *out, const pp_token *right, bool left_empty)
{
    if (is_end(right))
    {
        return;
    }
    if (left_empty || out->list.last == NULL)
    {
        append_list(state, out, right);
        return;
    }
    paste(state, out->list.last, &right->token);
    append_list(state, out, right->next);
}

/* Whether `, ## __VA_ARGS__` starts at a body's index. */
static bool is_comma_paste(const macro *definition, size_t index)
{
    const tb_shader_token *const body = definition->body;
    return definition->variadic && index + 2 < definition->body_count &&
           tb_shader_token_is(&body[index], ",") && tb_shader_token_is(&body[index + 1], "##") &&
           find_parameter(definition, &body[index + 2]) == (long)definition->parameter_count - 1;
}

/* A macro's body with its arguments in place: the tokens to read again. */
static pp_token *substitute(preprocessor *state, const macro *definition, argument *arguments,
                            const pp_token *invocation)
{
    expansion out = {0};
    list_init(&out.list);
    out.first = true;
    out.first_flags =
        invocation->token.flags & (TB_SHADER_TOKEN_LINE_START | TB_SHADER_TOKEN_SPACE_BEFORE);
    out.location = invocation->token.location;
    const tb_shader_token *const body = definition->body;
    const size_t count = definition->body_count;
    /* Whether the last operand gave no tokens, which `##` then pastes nothing onto. */
    bool last_empty = false;
    size_t index = 0;
    while (index < count && !state->stopped)
    {
        const tb_shader_token *const current = &body[index];
        const bool followed_by_paste =
            index + 1 < count && tb_shader_token_is(&body[index + 1], "##");
        if (definition->kind == MACRO_FUNCTION && tb_shader_token_is(current, "#"))
        {
            const long parameter = find_parameter(definition, &body[index + 1]);
            append_string(state, &out, current, arguments[parameter].raw);
            last_empty = false;
            index += 2;
            continue;
        }
        if (is_comma_paste(definition, index))
        {
            const pp_token *const rest = arguments[definition->parameter_count - 1].raw;
            if (!is_end(rest))
            {
                append_body_token(state, &out, current);
                append_list(state, &out, rest);
            }
            last_empty = is_end(rest);
            index += 3;
            continue;
        }
        if (tb_shader_token_is(current, "##"))
        {
            const tb_shader_token *const right = &body[index + 1];
            const long parameter = find_parameter(definition, right);
            if (parameter >= 0)
            {
                const pp_token *const raw = arguments[parameter].raw;
                paste_list(state, &out, raw, last_empty);
                last_empty = last_empty && is_end(raw);
            }
            else if (last_empty || out.list.last == NULL)
            {
                append_body_token(state, &out, right);
                last_empty = false;
            }
            else
            {
                paste(state, out.list.last, right);
            }
            index += 2;
            continue;
        }
        const long parameter = find_parameter(definition, current);
        if (parameter >= 0)
        {
            /* An operand of `##` is substituted as written, any other expanded. */
            const pp_token *const source = followed_by_paste
                                               ? arguments[parameter].raw
                                               : expanded_argument(state, &arguments[parameter]);
            const bool was_first = out.first;
            const unsigned first_flags = out.first_flags;
            if (!was_first)
            {
                /* The argument's first token is spaced as the parameter was. */
                out.first = true;
                out.first_flags = current->flags & TB_SHADER_TOKEN_SPACE_BEFORE;
            }
            append_list(state, &out, source);
            if (!was_first)
            {
                out.first = false;
                out.first_flags = first_flags;
            }
            last_empty = is_end(source);
            index++;
            continue;
        }
        append_body_token(state, &out, current);
        last_empty = false;
        index++;
    }
    return out.list.head;
}

/* Give each token of an expansion the hide set, and join it to the tokens after it. */
static pp_token *join_expansion(preprocessor *state, pp_token *expanded, const hideset *hidden,
                                pp_token *rest)
{
    if (expanded == NULL)
    {
        return rest;
    }
    pp_token *last = expanded;
    for (pp_token *token = expanded; token != NULL; token = token->next)
    {
        token->hidden = hideset_union(state, token->hidden, hidden);
        last = token;
    }
    last->next = rest;
    return expanded;
}

/* Whether a comma at the top of an invocation ends an argument: not inside
 * the variadic argument, which takes every comma. */
static bool comma_separates(const macro *definition, size_t completed)
{
    return !(definition->variadic && completed + 1 >= definition->parameter_count);
}

/* Read an invocation's arguments from the token after its `(`. Returns the
 * `)`, or NULL after an error, with *resume where reading goes on. */
static pp_token *read_arguments(preprocessor *state, const macro *definition, const pp_token *name,
                                pp_token *open, argument *arguments, pp_token **resume)
{
    const size_t wanted = definition->parameter_count;
    size_t count = 0;
    size_t depth = 0;
    pp_token *token = open->next;
    token_list current = {0};
    list_init(&current);
    for (;;)
    {
        if (is_end(token) || starts_directive(token))
        {
            ERROR_AT(state, name->token.location, "unterminated invocation of macro '%.*s'",
                     (int)name->token.length, name->token.text);
            *resume = token;
            return NULL;
        }
        const bool at_top = depth == 0;
        const bool closes = at_top && tb_shader_token_is(&token->token, ")");
        const bool separates =
            at_top && tb_shader_token_is(&token->token, ",") && comma_separates(definition, count);
        if (closes || separates)
        {
            if (count < wanted)
            {
                arguments[count].raw = list_finish(state, &current, token->token.location);
            }
            count++;
            list_init(&current);
            if (closes)
            {
                break;
            }
            token = token->next;
            continue;
        }
        if (tb_shader_token_is(&token->token, "("))
        {
            depth++;
        }
        else if (tb_shader_token_is(&token->token, ")"))
        {
            depth--;
        }
        list_append(&current, make_token(state, &token->token, token->pp_flags, token->hidden));
        token = token->next;
    }
    /* `F()` is one empty argument, which a macro of no parameters takes as none. */
    if (wanted == 0 && count == 1 && open->next == token)
    {
        count = 0;
    }
    /* A variadic macro may be given nothing for its `...`. */
    if (definition->variadic && count + 1 == wanted)
    {
        token_list none = {0};
        list_init(&none);
        arguments[count].raw = list_finish(state, &none, token->token.location);
        count++;
    }
    if (count != wanted)
    {
        ERROR_AT(state, name->token.location, "macro '%.*s' takes %zu argument%s, but %zu %s given",
                 (int)name->token.length, name->token.text, wanted, wanted == 1 ? "" : "s", count,
                 count == 1 ? "was" : "were");
        *resume = token->next;
        return NULL;
    }
    return token;
}

/* __LINE__ or __FILE__, where `name` is. */
static pp_token *builtin_expansion(preprocessor *state, const macro *definition,
                                   const pp_token *name)
{
    const tb_shader_location mapped = map_location(state, name->token.location);
    tb_shader_token made = name->token;
    if (definition->kind == MACRO_LINE)
    {
        made.kind = TB_SHADER_TOKEN_NUMBER;
        made.text = tb_fmt_string(&state->allocator, "%u", (unsigned)mapped.line);
    }
    else
    {
        made.kind = TB_SHADER_TOKEN_STRING;
        made.text =
            tb_fmt_string(&state->allocator, "\"%s\"", file_at(state, mapped.file)->path.text);
    }
    made.length = tb_str_length(made.text);
    return make_token(state, &made, PP_FROM_MACRO, name->hidden);
}

/* Expand the macro that `*cursor` names, if one may expand there: put its
 * expansion in front of what follows, point `*cursor` at it, and return true. */
static bool expand_macro(preprocessor *state, pp_token **cursor)
{
    pp_token *const name = *cursor;
    const macro *const definition = find_macro(state, &name->token);
    if (definition == NULL || hideset_contains(name->hidden, definition))
    {
        return false;
    }
    if (definition->kind == MACRO_LINE || definition->kind == MACRO_FILE)
    {
        pp_token *const made = builtin_expansion(state, definition, name);
        *cursor = join_expansion(state, made, NULL, name->next);
        return true;
    }
    if (definition->kind == MACRO_OBJECT)
    {
        const hideset *const hidden = hideset_add(state, name->hidden, definition);
        pp_token *const expanded = substitute(state, definition, NULL, name);
        *cursor = join_expansion(state, expanded, hidden, name->next);
        return true;
    }
    /* A function-like macro expands only where `(` follows its name. */
    pp_token *const open = name->next;
    if (!tb_shader_token_is(&open->token, "(") || starts_directive(open))
    {
        return false;
    }
    argument *const arguments =
        TB_ARENA_NEW_ARRAY(state->arena, argument, definition->parameter_count + 1);
    pp_token *resume = NULL;
    pp_token *const close = read_arguments(state, definition, name, open, arguments, &resume);
    if (close == NULL)
    {
        *cursor = resume;
        return true;
    }
    const hideset *hidden = hideset_intersection(state, name->hidden, close->hidden);
    hidden = hideset_add(state, hidden, definition);
    pp_token *const expanded = substitute(state, definition, arguments, name);
    *cursor = join_expansion(state, expanded, hidden, close->next);
    return true;
}

/* Expand every macro of a list that ends with END, as an argument or a
 * directive's line is expanded. Returns a new list that ends with END. */
static pp_token *expand_list(preprocessor *state, pp_token *list)
{
    token_list result = {0};
    list_init(&result);
    pp_token *token = list;
    if (state->nesting == MAX_EXPANSION_NESTING)
    {
        ERROR_AT(state, state->site, "macro invocations nest deeper than %zu",
                 MAX_EXPANSION_NESTING);
        state->stopped = true;
    }
    state->nesting++;
    while (!is_end(token) && !state->stopped)
    {
        if (expand_macro(state, &token))
        {
            continue;
        }
        pp_token *const next = token->next;
        list_append(&result, token);
        token = next;
    }
    state->nesting--;
    while (!is_end(token))
    {
        token = token->next;
    }
    return list_finish(state, &result, token->token.location);
}

/* -------------------------------------------------------------------------
 * Defining macros.
 * ------------------------------------------------------------------------- */

static bool is_reserved_name(const tb_shader_token *token)
{
    return token_equals(token, "defined") || token_equals(token, "__LINE__") ||
           token_equals(token, "__FILE__") || token_equals(token, "__VA_ARGS__");
}

static bool same_body_token(const tb_shader_token *a, const tb_shader_token *b, bool first)
{
    const bool spaced_alike = first || ((a->flags ^ b->flags) & TB_SHADER_TOKEN_SPACE_BEFORE) == 0;
    return a->kind == b->kind && a->length == b->length &&
           memcmp(a->text, b->text, a->length) == 0 && spaced_alike;
}

static bool same_definition(const macro *left, const macro *right)
{
    if (left->kind != right->kind || left->variadic != right->variadic ||
        left->parameter_count != right->parameter_count || left->body_count != right->body_count)
    {
        return false;
    }
    for (size_t index = 0; index < left->parameter_count; index++)
    {
        if (!name_equal(&left->parameters[index], &right->parameters[index]))
        {
            return false;
        }
    }
    for (size_t index = 0; index < left->body_count; index++)
    {
        if (!same_body_token(&left->body[index], &right->body[index], index == 0))
        {
            return false;
        }
    }
    return true;
}

/* The name of a parameter at `token`, or false after an error. */
static bool read_parameter(preprocessor *state, macro *definition, const pp_token *token,
                           const tb_array *parameters, name_key *parameter)
{
    if (tb_shader_token_is(&token->token, "..."))
    {
        definition->variadic = true;
        parameter->text = "__VA_ARGS__";
        parameter->length = tb_str_length(parameter->text);
        return true;
    }
    if (token->token.kind != TB_SHADER_TOKEN_IDENTIFIER || is_reserved_name(&token->token))
    {
        ERROR_AT(state, token->token.location, "expected a parameter name, not '%.*s'",
                 (int)token->token.length, token->token.text);
        return false;
    }
    *parameter = token_name(&token->token);
    for (size_t index = 0; index < tb_array_length(parameters); index++)
    {
        if (name_equal(tb_array_at(parameters, index), parameter))
        {
            ERROR_AT(state, token->token.location, "parameter '%.*s' is named twice",
                     (int)parameter->length, parameter->text);
            return false;
        }
    }
    return true;
}

/* Read a function-like macro's parameters, from the token after its `(`.
 * Returns the token after the `)`, or NULL after an error. */
static pp_token *read_parameters(preprocessor *state, macro *definition, pp_token *token)
{
    tb_array *const parameters = TB_ARRAY_CREATE(&state->allocator, name_key);
    if (continues_line(token) && tb_shader_token_is(&token->token, ")"))
    {
        definition->parameters = tb_array_data(parameters);
        return token->next;
    }
    for (;;)
    {
        if (!continues_line(token))
        {
            ERROR_AT(state, token->token.location, "missing ')' in the parameters of a macro");
            return NULL;
        }
        name_key parameter = {0};
        if (!read_parameter(state, definition, token, parameters, &parameter))
        {
            return NULL;
        }
        (void)tb_array_push(parameters, &parameter);
        token = token->next;
        if (continues_line(token) && tb_shader_token_is(&token->token, ")"))
        {
            definition->parameters = tb_array_data(parameters);
            definition->parameter_count = tb_array_length(parameters);
            return token->next;
        }
        if (!continues_line(token))
        {
            ERROR_AT(state, token->token.location, "missing ')' in the parameters of a macro");
            return NULL;
        }
        if (definition->variadic || !tb_shader_token_is(&token->token, ","))
        {
            ERROR_AT(state, token->token.location,
                     "expected ',' or ')' in the parameters of a macro");
            return NULL;
        }
        token = token->next;
    }
}

/* Check a body's `#` and `##` and its use of __VA_ARGS__. */
static bool check_body(preprocessor *state, const macro *definition)
{
    const size_t count = definition->body_count;
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_token *const token = &definition->body[index];
        const bool at_either_end = index == 0 || index + 1 == count;
        if (tb_shader_token_is(token, "##") && at_either_end)
        {
            ERROR_AT(state, token->location, "'##' cannot be at either end of a macro");
            return false;
        }
        const bool names_parameter =
            index + 1 < count && find_parameter(definition, &definition->body[index + 1]) >= 0;
        if (definition->kind == MACRO_FUNCTION && tb_shader_token_is(token, "#") &&
            !names_parameter)
        {
            ERROR_AT(state, token->location, "'#' is not followed by a macro parameter");
            return false;
        }
        if (token->kind == TB_SHADER_TOKEN_IDENTIFIER && token_equals(token, "__VA_ARGS__") &&
            !definition->variadic)
        {
            ERROR_AT(state, token->location, "__VA_ARGS__ is only for a variadic macro");
            return false;
        }
    }
    return true;
}

static bool check_macro_name(preprocessor *state, const pp_token *directive, const pp_token *name)
{
    if (!continues_line(name) || name->token.kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        ERROR_AT(state, directive->token.location, "#%.*s needs a macro name",
                 (int)directive->token.length, directive->token.text);
        return false;
    }
    if (is_reserved_name(&name->token))
    {
        ERROR_AT(state, name->token.location, "'%.*s' cannot be a macro's name",
                 (int)name->token.length, name->token.text);
        return false;
    }
    return true;
}

static void directive_define(preprocessor *state, const pp_token *directive)
{
    const pp_token *const name = directive->next;
    if (!check_macro_name(state, directive, name))
    {
        return;
    }
    macro *const definition = TB_ARENA_NEW(state->arena, macro);
    char *const text = tb_arena_alloc(state->arena, name->token.length + 1, 1);
    memcpy(text, name->token.text, name->token.length);
    text[name->token.length] = '\0';
    definition->name.text = text;
    definition->name.length = name->token.length;
    definition->kind = MACRO_OBJECT;
    pp_token *token = name->next;
    const bool function_like = continues_line(token) && tb_shader_token_is(&token->token, "(") &&
                               (token->token.flags & TB_SHADER_TOKEN_SPACE_BEFORE) == 0;
    if (function_like)
    {
        definition->kind = MACRO_FUNCTION;
        token = read_parameters(state, definition, token->next);
        if (token == NULL)
        {
            return;
        }
    }
    tb_array *const body = TB_ARRAY_CREATE(&state->allocator, tb_shader_token);
    while (continues_line(token))
    {
        (void)tb_array_push(body, &token->token);
        token = token->next;
    }
    definition->body = tb_array_data(body);
    definition->body_count = tb_array_length(body);
    if (!check_body(state, definition))
    {
        return;
    }
    const macro *const existing = tb_table_get(state->macros, &definition->name);
    if (existing != NULL && !same_definition(existing, definition))
    {
        WARNING_AT(state, name->token.location, "'%s' is redefined", definition->name.text);
    }
    (void)tb_table_remove(state->macros, &definition->name, NULL);
    (void)tb_table_put(state->macros, &definition->name, definition);
}

static void warn_extra_tokens(preprocessor *state, const pp_token *directive, const pp_token *extra)
{
    if (continues_line(extra))
    {
        WARNING_AT(state, extra->token.location, "extra tokens after #%.*s",
                   (int)directive->token.length, directive->token.text);
    }
}

static void directive_undef(preprocessor *state, const pp_token *directive)
{
    const pp_token *const name = directive->next;
    if (!check_macro_name(state, directive, name))
    {
        return;
    }
    const name_key key = token_name(&name->token);
    (void)tb_table_remove(state->macros, &key, NULL);
    warn_extra_tokens(state, directive, name->next);
}

/* -------------------------------------------------------------------------
 * Conditionals.
 * ------------------------------------------------------------------------- */

/* Replace `defined X` and `defined(X)` with 1 or 0, before anything expands.
 * Returns NULL after an error. */
static pp_token *replace_defined(preprocessor *state, pp_token *line)
{
    token_list list = {0};
    list_init(&list);
    pp_token *token = line;
    while (!is_end(token))
    {
        const bool is_defined = token->token.kind == TB_SHADER_TOKEN_IDENTIFIER &&
                                token_equals(&token->token, "defined");
        if (!is_defined)
        {
            pp_token *const next = token->next;
            list_append(&list, token);
            token = next;
            continue;
        }
        pp_token *operand = token->next;
        const bool parenthesized = tb_shader_token_is(&operand->token, "(");
        if (parenthesized)
        {
            operand = operand->next;
        }
        if (operand->token.kind != TB_SHADER_TOKEN_IDENTIFIER)
        {
            ERROR_AT(state, token->token.location, "'defined' needs a macro name");
            return NULL;
        }
        pp_token *after = operand->next;
        if (parenthesized && !tb_shader_token_is(&after->token, ")"))
        {
            ERROR_AT(state, token->token.location, "missing ')' after 'defined'");
            return NULL;
        }
        if (parenthesized)
        {
            after = after->next;
        }
        token->token.kind = TB_SHADER_TOKEN_NUMBER;
        token->token.text = find_macro(state, &operand->token) != NULL ? "1" : "0";
        token->token.length = 1;
        list_append(&list, token);
        token = after;
    }
    return list_finish(state, &list, token->token.location);
}

/* Evaluate the condition of `#if` or `#elif`, from the directive's name. */
static bool evaluate_condition(preprocessor *state, const pp_token *directive)
{
    pp_token *const line = copy_line(state, directive->next);
    if (is_end(line))
    {
        ERROR_AT(state, directive->token.location, "#%.*s needs an expression",
                 (int)directive->token.length, directive->token.text);
        return false;
    }
    pp_token *const replaced = replace_defined(state, line);
    if (replaced == NULL)
    {
        return false;
    }
    const pp_token *const expanded = expand_list(state, replaced);
    tb_array *const tokens = TB_ARRAY_CREATE(&state->allocator, tb_shader_token);
    for (const pp_token *token = expanded; !is_end(token); token = token->next)
    {
        (void)tb_array_push(tokens, &token->token);
    }
    int64_t value = 0;
    const bool evaluated =
        tb_shader_evaluate_condition(tb_array_data(tokens), tb_array_length(tokens),
                                     directive->token.location, report_error, state, &value);
    return evaluated && value != 0;
}

/* Whether the directive at `hash` has the name. */
static bool directive_is(const pp_token *hash, const char *name)
{
    const pp_token *const directive = hash->next;
    return continues_line(directive) && directive->token.kind == TB_SHADER_TOKEN_IDENTIFIER &&
           token_equals(&directive->token, name);
}

static bool opens_conditional(const pp_token *hash)
{
    return directive_is(hash, "if") || directive_is(hash, "ifdef") || directive_is(hash, "ifndef");
}

/* Skip a group whose condition is false: return the `#` of the `#elif`,
 * `#else` or `#endif` that ends it, or the end of the file. */
static pp_token *skip_group(pp_token *token)
{
    size_t depth = 0;
    while (!is_end(token))
    {
        if (!starts_directive(token))
        {
            token = token->next;
            continue;
        }
        const bool ends_group = directive_is(token, "elif") || directive_is(token, "else");
        if (opens_conditional(token))
        {
            depth++;
        }
        else if (directive_is(token, "endif") && depth > 0)
        {
            depth--;
        }
        else if (directive_is(token, "endif") || (depth == 0 && ends_group))
        {
            return token;
        }
        token = token->next;
    }
    return token;
}

/* The innermost conditional of the file being read, or NULL. */
static conditional *innermost(const preprocessor *state)
{
    const size_t depth = tb_array_length(state->conditionals);
    if (depth <= top_frame(state)->conditional_depth)
    {
        return NULL;
    }
    return tb_array_at(state->conditionals, depth - 1);
}

/* `#if`, `#ifdef` and `#ifndef`. Returns where reading goes on. */
static pp_token *directive_if(preprocessor *state, const pp_token *directive, pp_token *rest)
{
    bool value = false;
    if (token_equals(&directive->token, "if"))
    {
        value = evaluate_condition(state, directive);
    }
    else if (check_macro_name(state, directive, directive->next))
    {
        const bool defined = find_macro(state, &directive->next->token) != NULL;
        value = token_equals(&directive->token, "ifdef") ? defined : !defined;
        warn_extra_tokens(state, directive, directive->next->next);
    }
    const conditional entry = {directive->token.location, value, false};
    (void)tb_array_push(state->conditionals, &entry);
    return value ? rest : skip_group(rest);
}

static pp_token *directive_elif(preprocessor *state, const pp_token *directive, pp_token *rest)
{
    conditional *const entry = innermost(state);
    if (entry == NULL)
    {
        ERROR_AT(state, directive->token.location, "#elif without #if");
        return rest;
    }
    if (entry->seen_else)
    {
        ERROR_AT(state, directive->token.location, "#elif after #else");
        return skip_group(rest);
    }
    if (entry->taken)
    {
        return skip_group(rest);
    }
    entry->taken = evaluate_condition(state, directive);
    return entry->taken ? rest : skip_group(rest);
}

static pp_token *directive_else(preprocessor *state, const pp_token *directive, pp_token *rest)
{
    conditional *const entry = innermost(state);
    if (entry == NULL)
    {
        ERROR_AT(state, directive->token.location, "#else without #if");
        return rest;
    }
    warn_extra_tokens(state, directive, directive->next);
    if (entry->seen_else)
    {
        ERROR_AT(state, directive->token.location, "#else after #else");
        return skip_group(rest);
    }
    entry->seen_else = true;
    if (entry->taken)
    {
        return skip_group(rest);
    }
    entry->taken = true;
    return rest;
}

static void directive_endif(preprocessor *state, const pp_token *directive)
{
    if (innermost(state) == NULL)
    {
        ERROR_AT(state, directive->token.location, "#endif without #if");
        return;
    }
    tb_array_pop(state->conditionals, NULL);
    warn_extra_tokens(state, directive, directive->next);
}

/* -------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/* The index of a file by its path, added unread if it is new. */
static uint32_t file_index(preprocessor *state, const char *path, size_t length)
{
    const name_key key = {path, length};
    const uintptr_t found = (uintptr_t)tb_table_get(state->paths, &key);
    if (found != 0)
    {
        return (uint32_t)(found - 1);
    }
    source_file file = {0};
    char *const copy = tb_arena_alloc(state->arena, length + 1, 1);
    memcpy(copy, path, length);
    copy[length] = '\0';
    file.path.text = copy;
    file.path.length = length;
    const uint32_t index = (uint32_t)tb_array_length(state->files);
    (void)tb_array_push(state->files, &file);
    (void)tb_table_put(state->paths, &file_at(state, index)->path, (void *)(uintptr_t)(index + 1));
    return index;
}

/* Find a file among the request's own. */
static bool find_request_file(const tb_shader_preprocess_request *request, const char *path,
                              const char **text, size_t *length)
{
    for (size_t entry = 0; entry < request->file_count; entry++)
    {
        if (tb_str_equal(request->files[entry].path, path))
        {
            *text = request->files[entry].text;
            *length = request->files[entry].length;
            return true;
        }
    }
    return false;
}

/* Why a file cannot be read, or NULL when it can. */
static const char *load_problem(tb_shader_load_result loaded, const char *text, size_t length)
{
    if (loaded == TB_SHADER_LOAD_NOT_FOUND)
    {
        return "was not found";
    }
    if (loaded != TB_SHADER_LOAD_OK)
    {
        return "could not be read";
    }
    if (length > TB_SHADER_MAX_FILE_SIZE)
    {
        return "is too large";
    }
    if (length > 0 && memchr(text, '\0', length) != NULL)
    {
        return "holds a NUL byte";
    }
    return NULL;
}

/* Read a file and split it into tokens, once per compile. Returns false,
 * after reporting why, when it cannot be read. */
static bool load_file(preprocessor *state, uint32_t index, const tb_shader_location *included_at)
{
    if (file_at(state, index)->read)
    {
        return true;
    }
    const tb_shader_preprocess_request *const request = state->request;
    const char *const path = file_at(state, index)->path.text;
    const size_t path_length = file_at(state, index)->path.length;
    const char *text = NULL;
    size_t length = 0;
    tb_shader_load_result loaded = TB_SHADER_LOAD_NOT_FOUND;
    if (find_request_file(request, path, &text, &length))
    {
        loaded = TB_SHADER_LOAD_OK;
    }
    else if (request->loader != NULL)
    {
        loaded = request->loader->load(request->loader->context, path, path_length, state->arena,
                                       &text, &length);
    }
    if (text == NULL)
    {
        text = "";
        length = loaded == TB_SHADER_LOAD_OK ? 0 : length;
    }
    const char *const problem = load_problem(loaded, text, length);
    if (problem != NULL && included_at != NULL)
    {
        ERROR_AT(state, *included_at, "'%s' %s", path, problem);
        return false;
    }
    if (problem != NULL)
    {
        request_error(state, "'%s' %s", path, problem);
        return false;
    }
    tb_shader_token *tokens = NULL;
    const size_t count =
        tb_shader_lex(text, length, index, state->arena, report_error, state, &tokens);
    source_file *const file = file_at(state, index);
    file->read = true;
    file->text = text;
    file->length = length;
    file->tokens = tokens;
    file->token_count = count;
    return true;
}

/* A file's tokens as a new list, which ends with its PP_FILE_END token, joined to `rest`. */
static pp_token *file_tokens(preprocessor *state, uint32_t index, pp_token *rest)
{
    const source_file *const file = file_at(state, index);
    token_list list = {0};
    list_init(&list);
    for (size_t position = 0; position < file->token_count; position++)
    {
        const bool last = position + 1 == file->token_count;
        pp_token *const token =
            make_token(state, &file->tokens[position], last ? PP_FILE_END : 0u, NULL);
        if (token == NULL)
        {
            return rest;
        }
        list_append(&list, token);
    }
    if (list.last == NULL)
    {
        return rest;
    }
    list.last->next = rest;
    return list.head;
}

static bool push_frame(preprocessor *state, uint32_t file, tb_shader_location included_at)
{
    if (state->frame_count == TB_SHADER_MAX_INCLUDE_DEPTH)
    {
        ERROR_AT(state, included_at, "includes nest deeper than %zu files",
                 TB_SHADER_MAX_INCLUDE_DEPTH);
        state->stopped = true;
        return false;
    }
    const frame entry = {file, tb_array_length(state->conditionals), false, 0, file};
    state->frames[state->frame_count] = entry;
    state->frame_count++;
    return true;
}

/* The string an `#include` names, from its line. Returns NULL after an error. */
static const pp_token *include_operand(preprocessor *state, const pp_token *directive)
{
    const pp_token *operand = directive->next;
    if (!continues_line(operand))
    {
        ERROR_AT(state, directive->token.location, "#include needs a path");
        return NULL;
    }
    if (tb_shader_token_is(&operand->token, "<"))
    {
        ERROR_AT(state, operand->token.location,
                 "shader includes name virtual paths in quotes: #include \"/Engine/...\"");
        return NULL;
    }
    if (operand->token.kind != TB_SHADER_TOKEN_STRING)
    {
        operand = expand_list(state, copy_line(state, operand));
    }
    const bool quoted = operand->token.kind == TB_SHADER_TOKEN_STRING &&
                        (operand->token.flags & TB_SHADER_TOKEN_UNTERMINATED) == 0;
    if (!quoted)
    {
        ERROR_AT(state, directive->token.location, "#include needs a path in quotes");
        return NULL;
    }
    warn_extra_tokens(state, directive, operand->next);
    return operand;
}

/* `#include`. Returns where reading goes on: the included file's tokens, then `rest`. */
static pp_token *directive_include(preprocessor *state, const pp_token *directive, pp_token *rest)
{
    const pp_token *const operand = include_operand(state, directive);
    if (operand == NULL)
    {
        return rest;
    }
    const tb_shader_location location = operand->token.location;
    const char *const including = file_at(state, top_frame(state)->file)->path.text;
    size_t length = 0;
    const char *const path = tb_shader_path_resolve(
        including, operand->token.text + 1, operand->token.length - 2, state->arena, &length);
    if (path == NULL)
    {
        ERROR_AT(state, location, "%.*s is not a valid shader path", (int)operand->token.length,
                 operand->token.text);
        return rest;
    }
    const uint32_t index = file_index(state, path, length);
    if (!load_file(state, index, &location) || file_at(state, index)->once)
    {
        return rest;
    }
    if (!push_frame(state, index, location))
    {
        return rest;
    }
    return file_tokens(state, index, rest);
}

/* The number of `#line`, or 0 when it is not one from 1 to INT32_MAX. */
static uint32_t line_number(const pp_token *token)
{
    if (token->token.kind != TB_SHADER_TOKEN_NUMBER)
    {
        return 0;
    }
    uint64_t number = 0;
    for (size_t index = 0; index < token->token.length; index++)
    {
        const char byte = token->token.text[index];
        if (byte < '0' || byte > '9' || number > (uint64_t)INT32_MAX)
        {
            return 0;
        }
        number = number * 10 + (uint64_t)(byte - '0');
    }
    return number > (uint64_t)INT32_MAX ? 0 : (uint32_t)number;
}

/* `#line N` or `#line N "name"`: the line after it is line N. */
static void directive_line(preprocessor *state, const pp_token *directive)
{
    const pp_token *const line = expand_list(state, copy_line(state, directive->next));
    const uint32_t number = line_number(line);
    if (number == 0)
    {
        ERROR_AT(state, directive->token.location, "#line needs a line number from 1 to %d",
                 (int)INT32_MAX);
        return;
    }
    frame *const current = &state->frames[state->frame_count - 1];
    const pp_token *const name = line->next;
    if (!is_end(name))
    {
        const bool quoted = name->token.kind == TB_SHADER_TOKEN_STRING &&
                            (name->token.flags & TB_SHADER_TOKEN_UNTERMINATED) == 0;
        if (!quoted)
        {
            ERROR_AT(state, name->token.location, "#line names a file in quotes");
            return;
        }
        current->name = file_index(state, name->token.text + 1, name->token.length - 2);
    }
    else if (!current->renamed)
    {
        current->name = current->file;
    }
    /* The directive's last token says where it ends, line splices and all. */
    const pp_token *last = directive;
    while (continues_line(last->next))
    {
        last = last->next;
    }
    const uint32_t next_physical = last->token.location.line + 1;
    current->renamed = true;
    current->line_bias = (int64_t)number - (int64_t)next_physical;
}

/* The rest of a line's spelling, its tokens joined by a space where there was space. */
static const char *line_text(preprocessor *state, const pp_token *begin, size_t *length)
{
    size_t total = 0;
    for (const pp_token *token = begin; continues_line(token); token = token->next)
    {
        total += token->token.length + 1;
    }
    char *const text = tb_arena_alloc(state->arena, total + 1, 1);
    size_t written = 0;
    for (const pp_token *token = begin; continues_line(token); token = token->next)
    {
        if (token != begin && (token->token.flags & TB_SHADER_TOKEN_SPACE_BEFORE) != 0)
        {
            text[written] = ' ';
            written++;
        }
        memcpy(text + written, token->token.text, token->token.length);
        written += token->token.length;
    }
    text[written] = '\0';
    *length = written;
    return text;
}

static void emit(preprocessor *state, const tb_shader_token *token)
{
    if (tb_array_length(state->output) == state->max_tokens)
    {
        ERROR_AT(state, token->location, "the shader expands to more than %zu tokens",
                 state->max_tokens);
        state->stopped = true;
        return;
    }
    tb_shader_token emitted = *token;
    emitted.location = map_location(state, token->location);
    (void)tb_array_push(state->output, &emitted);
}

/* `#pragma once`, or a pragma passed on to the compiler. */
static void directive_pragma(preprocessor *state, const pp_token *hash, const pp_token *directive)
{
    const pp_token *const first = directive->next;
    const bool once = continues_line(first) && first->token.kind == TB_SHADER_TOKEN_IDENTIFIER &&
                      token_equals(&first->token, "once");
    if (once)
    {
        file_at(state, top_frame(state)->file)->once = true;
        warn_extra_tokens(state, directive, first->next);
        return;
    }
    tb_shader_token pragma = hash->token;
    pragma.kind = TB_SHADER_TOKEN_PRAGMA;
    pragma.flags = TB_SHADER_TOKEN_LINE_START;
    pragma.text = line_text(state, first, &pragma.length);
    emit(state, &pragma);
}

/* `#error` and `#warning`. */
static void directive_message(preprocessor *state, const pp_token *directive,
                              tb_shader_severity severity)
{
    size_t length = 0;
    const char *const text = line_text(state, directive->next, &length);
    report_at(state, severity, directive->token.location, "#%.*s%s%s", (int)directive->token.length,
              directive->token.text, length > 0 ? " " : "", text);
}

/* A directive, from its `#`. Returns where reading goes on. */
static pp_token *directive(preprocessor *state, pp_token *hash)
{
    pp_token *const name = hash->next;
    if (!continues_line(name))
    {
        return name; /* The null directive. */
    }
    pp_token *const rest = next_line(name);
    if (name->token.kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        ERROR_AT(state, name->token.location, "'%.*s' is not a directive", (int)name->token.length,
                 name->token.text);
        return rest;
    }
    if (token_equals(&name->token, "include"))
    {
        return directive_include(state, name, rest);
    }
    if (token_equals(&name->token, "if") || token_equals(&name->token, "ifdef") ||
        token_equals(&name->token, "ifndef"))
    {
        return directive_if(state, name, rest);
    }
    if (token_equals(&name->token, "elif"))
    {
        return directive_elif(state, name, rest);
    }
    if (token_equals(&name->token, "else"))
    {
        return directive_else(state, name, rest);
    }
    if (token_equals(&name->token, "endif"))
    {
        directive_endif(state, name);
    }
    else if (token_equals(&name->token, "define"))
    {
        directive_define(state, name);
    }
    else if (token_equals(&name->token, "undef"))
    {
        directive_undef(state, name);
    }
    else if (token_equals(&name->token, "line"))
    {
        directive_line(state, name);
    }
    else if (token_equals(&name->token, "pragma"))
    {
        directive_pragma(state, hash, name);
    }
    else if (token_equals(&name->token, "error"))
    {
        directive_message(state, name, TB_SHADER_ERROR);
    }
    else if (token_equals(&name->token, "warning"))
    {
        directive_message(state, name, TB_SHADER_WARNING);
    }
    else
    {
        ERROR_AT(state, name->token.location, "unknown directive '#%.*s'", (int)name->token.length,
                 name->token.text);
    }
    return rest;
}

/* The end of a file: report its unterminated conditionals, and close its frame. */
static void end_file(preprocessor *state)
{
    const frame *const current = top_frame(state);
    while (tb_array_length(state->conditionals) > current->conditional_depth)
    {
        conditional entry = {0};
        tb_array_pop(state->conditionals, &entry);
        ERROR_AT(state, entry.location, "unterminated conditional: #endif is missing");
    }
    state->frame_count--;
}

static void report_unterminated(preprocessor *state, const tb_shader_token *token)
{
    if (token->kind == TB_SHADER_TOKEN_STRING)
    {
        ERROR_AT(state, token->location, "unterminated string");
    }
    else
    {
        ERROR_AT(state, token->location, "unterminated character constant");
    }
}

static void run(preprocessor *state, pp_token *token)
{
    while (token != NULL && !state->stopped)
    {
        if (is_end(token))
        {
            if ((token->pp_flags & PP_FILE_END) != 0)
            {
                end_file(state);
            }
            token = token->next;
            continue;
        }
        if ((token->pp_flags & PP_FROM_MACRO) == 0)
        {
            state->site = token->token.location;
        }
        if (starts_directive(token))
        {
            token = directive(state, token);
            continue;
        }
        if (expand_macro(state, &token))
        {
            continue;
        }
        if ((token->token.flags & TB_SHADER_TOKEN_UNTERMINATED) != 0)
        {
            report_unterminated(state, &token->token);
        }
        emit(state, &token->token);
        token = token->next;
    }
}

/* -------------------------------------------------------------------------
 * The request.
 * ------------------------------------------------------------------------- */

static bool is_identifier(const char *text)
{
    const size_t length = tb_str_length(text);
    for (size_t index = 0; index < length; index++)
    {
        const char byte = text[index];
        const bool letter =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !(digit && index > 0))
        {
            return false;
        }
    }
    return length > 0;
}

static bool has_line_break(const char *text)
{
    return tb_str_find_any(text, 0, TB_STR_END, "\r\n") != TB_STR_NOT_FOUND;
}

static bool check_defines(preprocessor *state)
{
    const tb_shader_preprocess_request *const request = state->request;
    bool valid = true;
    for (size_t index = 0; index < request->define_count; index++)
    {
        const tb_shader_define *const define = &request->defines[index];
        if (define->name == NULL || !is_identifier(define->name))
        {
            request_error(state, "define '%s' is not an identifier",
                          define->name == NULL ? "" : define->name);
            valid = false;
        }
        else if (define->value != NULL && has_line_break(define->value))
        {
            request_error(state, "the value of define '%s' has a line break", define->name);
            valid = false;
        }
    }
    return valid;
}

static bool check_request(preprocessor *state)
{
    const tb_shader_preprocess_request *const request = state->request;
    bool valid = check_defines(state);
    if (request->path == NULL || !tb_shader_path_check(request->path, tb_str_length(request->path)))
    {
        request_error(state, "'%s' is not a valid shader path",
                      request->path == NULL ? "" : request->path);
        valid = false;
    }
    else if (!tb_str_ends_with(request->path, ".hlsl"))
    {
        request_error(state, "'%s' is not a .hlsl file, as a shader's main file must be",
                      request->path);
        valid = false;
    }
    for (size_t index = 0; index < request->file_count; index++)
    {
        const char *const path = request->files[index].path;
        if (path == NULL || !tb_shader_path_check(path, tb_str_length(path)))
        {
            request_error(state, "file '%s' does not have a valid shader path",
                          path == NULL ? "" : path);
            valid = false;
        }
    }
    return valid;
}

/* The request's defines as the text of a file of `#define` lines. */
static const char *defines_text(preprocessor *state, size_t *length)
{
    const tb_shader_preprocess_request *const request = state->request;
    tb_array *const text = TB_ARRAY_CREATE(&state->allocator, char);
    for (size_t index = 0; index < request->define_count; index++)
    {
        const tb_shader_define *const define = &request->defines[index];
        const char *const line = tb_fmt_string(&state->allocator, "#define %s %s\n", define->name,
                                               define->value == NULL ? "1" : define->value);
        tb_array_append(text, line, tb_str_length(line));
    }
    *length = tb_array_length(text);
    return *length == 0 ? "" : tb_array_data(text);
}

static void define_builtin(preprocessor *state, const char *name, macro_kind kind)
{
    macro *const definition = TB_ARENA_NEW(state->arena, macro);
    definition->name.text = name;
    definition->name.length = tb_str_length(name);
    definition->kind = kind;
    (void)tb_table_put(state->macros, &definition->name, definition);
}

/* Read the defines, then the main file. Returns OK or FAILED. */
static tb_shader_result preprocess(preprocessor *state)
{
    const tb_shader_preprocess_request *const request = state->request;
    define_builtin(state, "__LINE__", MACRO_LINE);
    define_builtin(state, "__FILE__", MACRO_FILE);
    const uint32_t main_file = file_index(state, request->path, tb_str_length(request->path));
    if (!load_file(state, main_file, NULL))
    {
        return TB_SHADER_FAILED;
    }
    /* The defines are a file of their own, which is not a dependency. */
    static const char defines_path[] = "<defines>";
    const uint32_t defines_file = file_index(state, defines_path, sizeof(defines_path) - 1);
    size_t defines_length = 0;
    const char *const defines = defines_text(state, &defines_length);
    tb_shader_token *tokens = NULL;
    const size_t count = tb_shader_lex(defines, defines_length, defines_file, state->arena,
                                       report_error, state, &tokens);
    source_file *const file = file_at(state, defines_file);
    file->text = defines;
    file->length = defines_length;
    file->tokens = tokens;
    file->token_count = count;

    const tb_shader_location nowhere = {0, 0, 0};
    (void)push_frame(state, main_file, nowhere);
    (void)push_frame(state, defines_file, nowhere);
    pp_token *const main_tokens = file_tokens(state, main_file, NULL);
    run(state, file_tokens(state, defines_file, main_tokens));
    return state->error_count > 0 ? TB_SHADER_FAILED : TB_SHADER_OK;
}

tb_shader_result tb_shader_preprocess_unit(const tb_shader_preprocess_request *request,
                                           tb_arena *arena, tb_shader_unit *unit)
{
    preprocessor state = {0};
    state.request = request;
    state.arena = arena;
    state.max_tokens = request->max_tokens == 0 ? TB_SHADER_MAX_TOKENS : request->max_tokens;
    state.allocator = tb_arena_allocator(arena);
    state.macros = tb_table_create(&state.allocator, 64, name_hash, name_equal);
    state.paths = tb_table_create(&state.allocator, 16, name_hash, name_equal);
    state.files = TB_ARRAY_CREATE(&state.allocator, source_file);
    state.conditionals = TB_ARRAY_CREATE(&state.allocator, conditional);
    state.output = TB_ARRAY_CREATE(&state.allocator, tb_shader_token);
    state.diagnostics = TB_ARRAY_CREATE(&state.allocator, tb_shader_diagnostic);

    const tb_shader_result result = check_request(&state) ? preprocess(&state) : TB_SHADER_INVALID;

    const size_t file_count = tb_array_length(state.files);
    const char **const paths = TB_ARENA_NEW_ARRAY(arena, const char *, file_count + 1);
    bool *const read = TB_ARENA_NEW_ARRAY(arena, bool, file_count + 1);
    for (size_t index = 0; index < file_count; index++)
    {
        const source_file *const file = file_at(&state, (uint32_t)index);
        paths[index] = file->path.text;
        read[index] = file->read;
    }
    unit->tokens = tb_array_data(state.output);
    unit->token_count = tb_array_length(state.output);
    unit->files = paths;
    unit->files_read = read;
    unit->file_count = file_count;
    unit->diagnostics = tb_array_data(state.diagnostics);
    unit->diagnostic_count = tb_array_length(state.diagnostics);
    unit->error_count = state.error_count;
    return result;
}
