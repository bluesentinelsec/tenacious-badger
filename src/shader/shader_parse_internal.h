/* The parser's state and the functions its files share: reading tokens,
 * reporting errors and recovering from them, and the names in scope that
 * tell a type from a value. */
#ifndef TB_SHADER_PARSE_INTERNAL_H
#define TB_SHADER_PARSE_INTERNAL_H

#include "shader_syntax.h"

#include "badger/allocator.h"
#include "badger/array.h"
#include "badger/table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a name in scope is: enough to parse, not yet to check. */
typedef enum tb_shader_name_kind
{
    TB_SHADER_NAME_UNKNOWN,           /* Not declared. */
    TB_SHADER_NAME_VALUE,             /* A variable, a function, an enumerator. */
    TB_SHADER_NAME_TYPE,              /* A type that takes no template arguments. */
    TB_SHADER_NAME_TEMPLATE_TYPE,     /* A type that may take them: `Texture2D`, `vector`. */
    TB_SHADER_NAME_TEMPLATE_FUNCTION, /* A function declared with `template<...>`. */
    TB_SHADER_NAME_NAMESPACE
} tb_shader_name_kind;

typedef struct tb_shader_parser
{
    const tb_shader_unit *unit;
    tb_arena *arena;
    tb_allocator allocator;
    const tb_shader_token *tokens;
    size_t count;
    size_t position;
    /* The first `>` of a `>>` has closed a template argument list, and the
     * current token is the second. */
    bool split_shift;
    tb_shader_token half_shift;
    tb_shader_token end; /* What is read past the last token. */

    tb_array *diagnostics;
    size_t error_count;
    bool panic;   /* An error was reported, and the parser has not yet recovered. */
    bool stopped; /* Too many errors, or nesting too deep: every read is the end. */
    size_t depth;

    /* Names declared at namespace scope, by their qualified name, and the
     * built-in types. Values are tb_shader_name_kind. */
    tb_table *names;
    /* Names declared in a function, a local struct, or a template's
     * parameters, with what each replaced, so a scope can be undone. */
    tb_table *scoped;
    tb_array *scope_changes;
    size_t local_depth; /* Function bodies and parameter lists open. */
    /* The namespaces and structs being declared in, as `N::S`; empty at the top. */
    tb_shader_name path;
    /* When above zero, `>` ends a template argument list instead of comparing. */
    size_t angle_depth;
    /* The layout the last `#pragma pack_matrix` named, or zero. */
    uint64_t pack_matrix;
} tb_shader_parser;

/* Tokens. */
const tb_shader_token *tb_shader_peek(tb_shader_parser *parser);
const tb_shader_token *tb_shader_peek_at(tb_shader_parser *parser, size_t ahead);
void tb_shader_advance(tb_shader_parser *parser);
bool tb_shader_at_end(tb_shader_parser *parser);
bool tb_shader_is(tb_shader_parser *parser, const char *spelling);
bool tb_shader_accept(tb_shader_parser *parser, const char *spelling);
/* Consume `spelling`, or report that it is missing and return false. */
bool tb_shader_expect(tb_shader_parser *parser, const char *spelling);
/* Consume the `>` that closes a template argument list, splitting a `>>`. */
bool tb_shader_expect_closing_angle(tb_shader_parser *parser);
bool tb_shader_is_keyword(const tb_shader_token *token);
bool tb_shader_token_equals(const tb_shader_token *token, const char *text);
tb_shader_name tb_shader_token_name(const tb_shader_token *token);

/* Errors and recovery. */
void tb_shader_parse_error(tb_shader_parser *parser, const tb_shader_token *at, const char *format,
                           ...);
/* Report that the current token is not what `what` names: "expected a name". */
void tb_shader_parse_expected(tb_shader_parser *parser, const char *what);
/* Skip to the end of the statement or declaration the error is in: past a
 * `;`, or to a `}` that closes an enclosing block. */
void tb_shader_recover(tb_shader_parser *parser);
/* Enter a construct that may nest; false once nesting is too deep. */
bool tb_shader_enter(tb_shader_parser *parser);
void tb_shader_leave(tb_shader_parser *parser);

/* Nodes and names. */
tb_shader_node *tb_shader_new_node(tb_shader_parser *parser, tb_shader_node_kind kind,
                                   tb_shader_location location);
tb_shader_type_syntax *tb_shader_new_type_syntax(tb_shader_parser *parser,
                                                 tb_shader_type_syntax_kind kind,
                                                 tb_shader_location location);
tb_shader_name tb_shader_join_names(tb_shader_parser *parser, tb_shader_name left,
                                    const char *separator, tb_shader_name right);

/* A list being built, linked by `next`. */
typedef struct tb_shader_node_list
{
    tb_shader_node *first;
    tb_shader_node *last;
    size_t count;
} tb_shader_node_list;

void tb_shader_list_append(tb_shader_node_list *list, tb_shader_node *node);

/* Scopes. */
size_t tb_shader_push_scope(tb_shader_parser *parser);
void tb_shader_pop_scope(tb_shader_parser *parser, size_t mark);
/* Declare a name where the parser is: in the innermost local scope, or in
 * the namespace or struct being declared in. */
void tb_shader_declare(tb_shader_parser *parser, tb_shader_name name, tb_shader_name_kind kind);
/* Declare a name in the innermost scope opened by tb_shader_push_scope(),
 * as a template's parameters are. */
void tb_shader_declare_scoped(tb_shader_parser *parser, tb_shader_name name,
                              tb_shader_name_kind kind);
tb_shader_name_kind tb_shader_lookup(tb_shader_parser *parser, tb_shader_name name);
/* Enter a namespace or struct named `name`, and return the path to restore. */
tb_shader_name tb_shader_enter_path(tb_shader_parser *parser, tb_shader_name name);

/* Reading names without consuming them: the index just past a possibly
 * qualified name that starts at `index` (`::N::Light`), or `index` when no
 * name starts there. */
size_t tb_shader_scan_name(tb_shader_parser *parser, size_t index);
/* The name the tokens [begin, end) spell, as tb_shader_scan_name() found it. */
tb_shader_name tb_shader_scanned_name(tb_shader_parser *parser, size_t begin, size_t end);
/* The index just past a type that starts at `index`, its template arguments
 * included, or `index` when no type starts there. */
size_t tb_shader_scan_type(tb_shader_parser *parser, size_t index);
/* The index just past a balanced `<...>` at `index`, or `index` when there is none. */
size_t tb_shader_scan_angles(tb_shader_parser *parser, size_t index);

/* Declarations (shader_parse_declaration.c). */
/* Parse declarations at namespace scope into `list`: to the end of the
 * tokens, or to the `}` of a namespace when `in_braces`, which is left. */
void tb_shader_parse_declarations(tb_shader_parser *parser, tb_shader_node_list *list,
                                  bool in_braces);
/* Whether the current token starts a declaration rather than an expression. */
bool tb_shader_starts_declaration(tb_shader_parser *parser);
/* A modifier the current token spells, or 0. */
uint64_t tb_shader_modifier_at(tb_shader_parser *parser);
/* Parse one declaration where a statement may be, and append what it
 * declares to `list`. */
void tb_shader_parse_declaration(tb_shader_parser *parser, tb_shader_node_list *list,
                                 tb_shader_attribute *attributes);
tb_shader_attribute *tb_shader_parse_attributes(tb_shader_parser *parser);
tb_shader_type_syntax *tb_shader_parse_type(tb_shader_parser *parser,
                                            tb_shader_node_list *definitions);

/* Statements (shader_parse_statement.c). */
tb_shader_node *tb_shader_parse_block(tb_shader_parser *parser);

/* Expressions (shader_parse_expression.c). */
tb_shader_node *tb_shader_parse_expression(tb_shader_parser *parser);
tb_shader_node *tb_shader_parse_assignment(tb_shader_parser *parser);
tb_shader_node *tb_shader_parse_conditional(tb_shader_parser *parser);
tb_shader_node *tb_shader_parse_initializer(tb_shader_parser *parser);
/* `(a, b)`, after the `(`: the arguments up to and including the `)`. */
tb_shader_node *tb_shader_parse_arguments(tb_shader_parser *parser, size_t *count);
/* `<a, b>` of a type or a function: the arguments, after the `<`, up to and
 * including the `>`. */
tb_shader_template_argument *tb_shader_parse_template_arguments(tb_shader_parser *parser,
                                                                size_t *count);

#endif
