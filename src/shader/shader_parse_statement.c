/* Statements: blocks, declarations, expressions, and the statements that
 * branch, loop, switch and return. */
#include "shader_parse_internal.h"

static tb_shader_node *parse_statement(tb_shader_parser *parser);

static bool is_word(const tb_shader_token *token, const char *word)
{
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && tb_shader_token_equals(token, word);
}

/* A keyword's statement, after the keyword: `break;`, `continue;`, `discard;`. */
static tb_shader_node *parse_simple(tb_shader_parser *parser, tb_shader_node_kind kind)
{
    tb_shader_node *const node = tb_shader_new_node(parser, kind, tb_shader_peek(parser)->location);
    tb_shader_advance(parser);
    (void)tb_shader_expect(parser, ";");
    return node;
}

/* `(condition)` of `if`, `while`, `do` and `switch`. */
static tb_shader_node *parse_parenthesized(tb_shader_parser *parser)
{
    if (!tb_shader_expect(parser, "("))
    {
        return NULL;
    }
    tb_shader_node *const condition = tb_shader_parse_expression(parser);
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ")");
    }
    return condition;
}

static tb_shader_node *parse_if(tb_shader_parser *parser, tb_shader_node *node)
{
    node->condition = parse_parenthesized(parser);
    if (parser->panic)
    {
        return node;
    }
    node->body = parse_statement(parser);
    if (tb_shader_accept(parser, "else"))
    {
        node->else_body = parse_statement(parser);
    }
    return node;
}

static tb_shader_node *parse_while(tb_shader_parser *parser, tb_shader_node *node)
{
    node->condition = parse_parenthesized(parser);
    if (!parser->panic)
    {
        node->body = parse_statement(parser);
    }
    return node;
}

static tb_shader_node *parse_do(tb_shader_parser *parser, tb_shader_node *node)
{
    node->body = parse_statement(parser);
    if (parser->panic || !tb_shader_expect(parser, "while"))
    {
        return node;
    }
    node->condition = parse_parenthesized(parser);
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
    return node;
}

static tb_shader_node *parse_declaration_statement(tb_shader_parser *parser,
                                                   tb_shader_attribute *attributes)
{
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_DECLARATION, tb_shader_peek(parser)->location);
    tb_shader_node_list declared = {0};
    tb_shader_parse_declaration(parser, &declared, attributes);
    node->children = declared.first;
    node->child_count = declared.count;
    return node;
}

static tb_shader_node *parse_expression_statement(tb_shader_parser *parser)
{
    tb_shader_node *const node = tb_shader_new_node(parser, TB_SHADER_NODE_EXPRESSION_STATEMENT,
                                                    tb_shader_peek(parser)->location);
    node->value = tb_shader_parse_expression(parser);
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
    return node;
}

/* `for (init; condition; step) body`; the names `init` declares are the loop's. */
static tb_shader_node *parse_for(tb_shader_parser *parser, tb_shader_node *node)
{
    if (!tb_shader_expect(parser, "("))
    {
        return node;
    }
    const size_t mark = tb_shader_push_scope(parser);
    if (tb_shader_is(parser, ";"))
    {
        tb_shader_advance(parser);
    }
    else if (tb_shader_starts_declaration(parser))
    {
        node->init = parse_declaration_statement(parser, NULL);
    }
    else
    {
        node->init = parse_expression_statement(parser);
    }
    if (!parser->panic && !tb_shader_is(parser, ";"))
    {
        node->condition = tb_shader_parse_expression(parser);
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
    if (!parser->panic && !tb_shader_is(parser, ")"))
    {
        node->step = tb_shader_parse_expression(parser);
    }
    if (!parser->panic && tb_shader_expect(parser, ")"))
    {
        node->body = parse_statement(parser);
    }
    tb_shader_pop_scope(parser, mark);
    return node;
}

static tb_shader_node *parse_return(tb_shader_parser *parser, tb_shader_node *node)
{
    if (!tb_shader_is(parser, ";"))
    {
        node->value = tb_shader_parse_expression(parser);
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
    return node;
}

static tb_shader_node *parse_case(tb_shader_parser *parser, tb_shader_node *node)
{
    if (node->kind == TB_SHADER_NODE_CASE)
    {
        node->value = tb_shader_parse_conditional(parser);
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ":");
    }
    return node;
}

typedef struct keyword_statement
{
    const char *word;
    tb_shader_node_kind kind;
    tb_shader_node *(*parse)(tb_shader_parser *parser, tb_shader_node *node);
} keyword_statement;

static const keyword_statement keyword_statements[] = {
    {"if", TB_SHADER_NODE_IF, parse_if},
    {"for", TB_SHADER_NODE_FOR, parse_for},
    {"while", TB_SHADER_NODE_WHILE, parse_while},
    {"do", TB_SHADER_NODE_DO, parse_do},
    {"switch", TB_SHADER_NODE_SWITCH, parse_while},
    {"case", TB_SHADER_NODE_CASE, parse_case},
    {"default", TB_SHADER_NODE_DEFAULT, parse_case},
    {"return", TB_SHADER_NODE_RETURN, parse_return},
};

static tb_shader_node *parse_keyword_statement(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    for (size_t index = 0; index < sizeof(keyword_statements) / sizeof(keyword_statements[0]);
         index++)
    {
        const keyword_statement *const statement = &keyword_statements[index];
        if (is_word(token, statement->word))
        {
            tb_shader_node *const node =
                tb_shader_new_node(parser, statement->kind, token->location);
            tb_shader_advance(parser);
            return statement->parse(parser, node);
        }
    }
    if (is_word(token, "break"))
    {
        return parse_simple(parser, TB_SHADER_NODE_BREAK);
    }
    if (is_word(token, "continue"))
    {
        return parse_simple(parser, TB_SHADER_NODE_CONTINUE);
    }
    if (is_word(token, "discard"))
    {
        return parse_simple(parser, TB_SHADER_NODE_DISCARD);
    }
    return NULL;
}

static tb_shader_node *parse_unattributed(tb_shader_parser *parser, tb_shader_attribute *attributes)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (tb_shader_is(parser, "{"))
    {
        return tb_shader_parse_block(parser);
    }
    if (tb_shader_is(parser, ";"))
    {
        tb_shader_advance(parser);
        return tb_shader_new_node(parser, TB_SHADER_NODE_EMPTY, token->location);
    }
    if (token->kind == TB_SHADER_TOKEN_PRAGMA)
    {
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_PRAGMA, token->location);
        node->name = tb_shader_token_name(token);
        tb_shader_advance(parser);
        return node;
    }
    tb_shader_node *const keyword = parse_keyword_statement(parser);
    if (keyword != NULL)
    {
        return keyword;
    }
    if (tb_shader_starts_declaration(parser))
    {
        return parse_declaration_statement(parser, attributes);
    }
    return parse_expression_statement(parser);
}

static tb_shader_node *parse_statement(tb_shader_parser *parser)
{
    if (!tb_shader_enter(parser))
    {
        return NULL;
    }
    tb_shader_attribute *const attributes = tb_shader_parse_attributes(parser);
    tb_shader_node *statement = NULL;
    if (!parser->panic)
    {
        statement = parse_unattributed(parser, attributes);
    }
    if (statement != NULL && statement->attributes == NULL)
    {
        statement->attributes = attributes;
    }
    tb_shader_leave(parser);
    return statement;
}

tb_shader_node *tb_shader_parse_block(tb_shader_parser *parser)
{
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_BLOCK, tb_shader_peek(parser)->location);
    if (!tb_shader_expect(parser, "{") || !tb_shader_enter(parser))
    {
        return node;
    }
    const size_t mark = tb_shader_push_scope(parser);
    parser->local_depth++;
    tb_shader_node_list statements = {0};
    while (!tb_shader_at_end(parser) && !tb_shader_is(parser, "}"))
    {
        const size_t before = parser->position;
        tb_shader_node *const statement = parse_statement(parser);
        if (statement != NULL)
        {
            tb_shader_list_append(&statements, statement);
        }
        if (parser->panic)
        {
            tb_shader_recover(parser);
        }
        if (parser->position == before && !tb_shader_is(parser, "}"))
        {
            tb_shader_advance(parser);
        }
    }
    (void)tb_shader_expect(parser, "}");
    parser->local_depth--;
    tb_shader_pop_scope(parser, mark);
    tb_shader_leave(parser);
    node->children = statements.first;
    node->child_count = statements.count;
    return node;
}
