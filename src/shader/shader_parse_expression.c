/* Expressions, with HLSL's precedence, which is C's: literals, names,
 * constructors, casts, calls, members and swizzles, indexing, the unary,
 * binary, conditional and assignment operators, and initializer lists. */
#include "shader_parse_internal.h"

#include "badger/str.h"

static tb_shader_node *parse_unary(tb_shader_parser *parser);

/* -------------------------------------------------------------------------
 * Operators.
 * ------------------------------------------------------------------------- */

typedef struct operator_spelling
{
    const char *text;
    tb_shader_operator op;
    int precedence; /* Binary operators: higher binds tighter. */
} operator_spelling;

static const operator_spelling binary_operators[] = {
    {"||", TB_SHADER_OP_OR, 1},
    {"&&", TB_SHADER_OP_AND, 2},
    {"|", TB_SHADER_OP_BIT_OR, 3},
    {"^", TB_SHADER_OP_BIT_XOR, 4},
    {"&", TB_SHADER_OP_BIT_AND, 5},
    {"==", TB_SHADER_OP_EQUAL, 6},
    {"!=", TB_SHADER_OP_NOT_EQUAL, 6},
    {"<", TB_SHADER_OP_LESS, 7},
    {">", TB_SHADER_OP_GREATER, 7},
    {"<=", TB_SHADER_OP_LESS_EQUAL, 7},
    {">=", TB_SHADER_OP_GREATER_EQUAL, 7},
    {"<<", TB_SHADER_OP_SHIFT_LEFT, 8},
    {">>", TB_SHADER_OP_SHIFT_RIGHT, 8},
    {"+", TB_SHADER_OP_ADD, 9},
    {"-", TB_SHADER_OP_SUBTRACT, 9},
    {"*", TB_SHADER_OP_MULTIPLY, 10},
    {"/", TB_SHADER_OP_DIVIDE, 10},
    {"%", TB_SHADER_OP_REMAINDER, 10},
};

static const operator_spelling assignment_operators[] = {
    {"=", TB_SHADER_OP_ASSIGN, 0},
    {"*=", TB_SHADER_OP_MULTIPLY_ASSIGN, 0},
    {"/=", TB_SHADER_OP_DIVIDE_ASSIGN, 0},
    {"%=", TB_SHADER_OP_REMAINDER_ASSIGN, 0},
    {"+=", TB_SHADER_OP_ADD_ASSIGN, 0},
    {"-=", TB_SHADER_OP_SUBTRACT_ASSIGN, 0},
    {"<<=", TB_SHADER_OP_SHIFT_LEFT_ASSIGN, 0},
    {">>=", TB_SHADER_OP_SHIFT_RIGHT_ASSIGN, 0},
    {"&=", TB_SHADER_OP_BIT_AND_ASSIGN, 0},
    {"^=", TB_SHADER_OP_BIT_XOR_ASSIGN, 0},
    {"|=", TB_SHADER_OP_BIT_OR_ASSIGN, 0},
};

static const operator_spelling unary_operators[] = {
    {"+", TB_SHADER_OP_PLUS, 0},
    {"-", TB_SHADER_OP_NEGATE, 0},
    {"!", TB_SHADER_OP_NOT, 0},
    {"~", TB_SHADER_OP_BIT_NOT, 0},
    {"++", TB_SHADER_OP_PRE_INCREMENT, 0},
    {"--", TB_SHADER_OP_PRE_DECREMENT, 0},
};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

const char *tb_shader_operator_spelling(tb_shader_operator op)
{
    if (op == TB_SHADER_OP_POST_INCREMENT)
    {
        return "++";
    }
    if (op == TB_SHADER_OP_POST_DECREMENT)
    {
        return "--";
    }
    if (op == TB_SHADER_OP_COMMA)
    {
        return ",";
    }
    const operator_spelling *const tables[] = {binary_operators, assignment_operators,
                                               unary_operators};
    const size_t counts[] = {COUNT_OF(binary_operators), COUNT_OF(assignment_operators),
                             COUNT_OF(unary_operators)};
    for (size_t table = 0; table < COUNT_OF(tables); table++)
    {
        for (size_t index = 0; index < counts[table]; index++)
        {
            if (tables[table][index].op == op)
            {
                return tables[table][index].text;
            }
        }
    }
    return "?";
}

/* Whether a `>` in an operator ends a template argument list instead. */
static bool closes_angles(const tb_shader_parser *parser, const char *text)
{
    return parser->angle_depth > 0 && text[0] == '>';
}

static const operator_spelling *operator_at(tb_shader_parser *parser,
                                            const operator_spelling *table, size_t count)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (token->kind != TB_SHADER_TOKEN_PUNCTUATOR)
    {
        return NULL;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (tb_shader_token_is(token, table[index].text) &&
            !closes_angles(parser, table[index].text))
        {
            return &table[index];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * The height of an expression.
 * ------------------------------------------------------------------------- */

static uint32_t height_of(const tb_shader_node *node)
{
    return node == NULL ? 0 : node->height;
}

/* Record a node's height, and stop the parse when it is too tall. */
static tb_shader_node *finish(tb_shader_parser *parser, tb_shader_node *node)
{
    uint32_t tallest = height_of(node->left);
    const uint32_t parts[] = {height_of(node->right), height_of(node->condition)};
    for (size_t index = 0; index < sizeof(parts) / sizeof(parts[0]); index++)
    {
        tallest = parts[index] > tallest ? parts[index] : tallest;
    }
    for (const tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        tallest = child->height > tallest ? child->height : tallest;
    }
    node->height = tallest + 1;
    if (node->height > TB_SHADER_MAX_EXPRESSION_HEIGHT && !parser->stopped)
    {
        tb_shader_token at = {0};
        at.location = node->location;
        parser->panic = false;
        tb_shader_parse_error(parser, &at, "the expression is more than %u operations deep",
                              (unsigned)TB_SHADER_MAX_EXPRESSION_HEIGHT);
        parser->stopped = true;
    }
    return node;
}

/* -------------------------------------------------------------------------
 * Literals.
 * ------------------------------------------------------------------------- */

static bool is_digit(char byte, unsigned base)
{
    if (base == 16)
    {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
               (byte >= 'A' && byte <= 'F');
    }
    return byte >= '0' && byte < (char)('0' + base);
}

static unsigned digit_value(char byte)
{
    if (byte >= 'a')
    {
        return (unsigned)(byte - 'a' + 10);
    }
    if (byte >= 'A')
    {
        return (unsigned)(byte - 'A' + 10);
    }
    return (unsigned)(byte - '0');
}

/* Whether a suffix is one an integer may have: at most one `u` and at most
 * two `l`s, as `1u`, `1ul`, `1ull` and `1lu`. */
static bool is_integer_suffix(const char *text, size_t length)
{
    size_t unsigned_count = 0;
    size_t long_count = 0;
    for (size_t index = 0; index < length; index++)
    {
        const char byte = text[index];
        if (byte == 'u' || byte == 'U')
        {
            unsigned_count++;
        }
        else if (byte == 'l' || byte == 'L')
        {
            long_count++;
        }
        else
        {
            return false;
        }
    }
    return unsigned_count <= 1 && long_count <= 2;
}

static bool is_float_suffix(const char *text, size_t length)
{
    if (length == 0)
    {
        return true;
    }
    return length == 1 && tb_str_find_char("fFhHlL", 0, TB_STR_END, text[0]) != TB_STR_NOT_FOUND;
}

/* The end of a decimal floating-point number's digits, exponent included,
 * from `index`; 0 when it is not one. */
static size_t float_end(const char *text, size_t length)
{
    size_t index = 0;
    size_t digits = 0;
    while (index < length && is_digit(text[index], 10))
    {
        index++;
        digits++;
    }
    const bool has_point = index < length && text[index] == '.';
    if (has_point)
    {
        index++;
        while (index < length && is_digit(text[index], 10))
        {
            index++;
            digits++;
        }
    }
    if (digits == 0)
    {
        return 0;
    }
    const bool has_exponent = index < length && (text[index] == 'e' || text[index] == 'E');
    if (has_exponent)
    {
        index++;
        if (index < length && (text[index] == '+' || text[index] == '-'))
        {
            index++;
        }
        const size_t exponent_begin = index;
        while (index < length && is_digit(text[index], 10))
        {
            index++;
        }
        if (index == exponent_begin)
        {
            return 0;
        }
    }
    return has_point || has_exponent ? index : 0;
}

static tb_shader_node *parse_float(tb_shader_parser *parser, const tb_shader_token *token,
                                   size_t end)
{
    tb_shader_node *const node = tb_shader_new_node(parser, TB_SHADER_NODE_FLOAT, token->location);
    node->name = tb_shader_token_name(token);
    node->suffix.text = token->text + end;
    node->suffix.length = token->length - end;
    if (!is_float_suffix(node->suffix.text, node->suffix.length))
    {
        tb_shader_parse_error(parser, token, "invalid suffix '%.*s' on a floating-point number",
                              (int)node->suffix.length, node->suffix.text);
    }
    return finish(parser, node);
}

static tb_shader_node *parse_integer(tb_shader_parser *parser, const tb_shader_token *token)
{
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_INTEGER, token->location);
    node->name = tb_shader_token_name(token);
    const char *const text = token->text;
    const size_t length = token->length;
    unsigned base = 10;
    size_t index = 0;
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        index = 2;
    }
    else if (length > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
    {
        base = 2;
        index = 2;
    }
    else if (length > 1 && text[0] == '0')
    {
        base = 8;
    }
    const size_t digits_begin = index;
    uint64_t value = 0;
    bool overflow = false;
    while (index < length && is_digit(text[index], base == 8 ? 10 : base))
    {
        const unsigned digit = digit_value(text[index]);
        if (digit >= base)
        {
            tb_shader_parse_error(parser, token, "invalid digit '%c' in an octal number",
                                  text[index]);
            return finish(parser, node);
        }
        overflow = overflow || value > (UINT64_MAX - digit) / base;
        value = value * base + digit;
        index++;
    }
    node->integer = value;
    node->suffix.text = text + index;
    node->suffix.length = length - index;
    if (index == digits_begin || !is_integer_suffix(node->suffix.text, node->suffix.length))
    {
        tb_shader_parse_error(parser, token, "invalid suffix '%.*s' on an integer",
                              (int)node->suffix.length, node->suffix.text);
    }
    else if (overflow)
    {
        tb_shader_parse_error(parser, token, "the integer '%.*s' does not fit in 64 bits",
                              (int)length, text);
    }
    return finish(parser, node);
}

static tb_shader_node *parse_number(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    tb_shader_advance(parser);
    const bool is_prefixed =
        token->length > 2 && token->text[0] == '0' &&
        tb_str_find_char("xXbB", 0, TB_STR_END, token->text[1]) != TB_STR_NOT_FOUND;
    const size_t end = is_prefixed ? 0 : float_end(token->text, token->length);
    if (end > 0)
    {
        return parse_float(parser, token, end);
    }
    return parse_integer(parser, token);
}

/* -------------------------------------------------------------------------
 * Primary expressions.
 * ------------------------------------------------------------------------- */

static void skip_to(tb_shader_parser *parser, size_t index)
{
    while (parser->position < index && !tb_shader_at_end(parser))
    {
        tb_shader_advance(parser);
    }
}

static bool is_word(const tb_shader_token *token, const char *word)
{
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && tb_shader_token_equals(token, word);
}

/* Whether `<` at the current token opens the template arguments of a call,
 * `Load<float4>(offset)`: it holds a type first and a `(` follows it. */
static bool at_call_template_arguments(tb_shader_parser *parser)
{
    if (!tb_shader_is(parser, "<"))
    {
        return false;
    }
    const size_t first = parser->position + 1;
    if (tb_shader_scan_type(parser, first) == first)
    {
        return false;
    }
    const size_t end = tb_shader_scan_angles(parser, parser->position);
    if (end == parser->position)
    {
        return false;
    }
    return tb_shader_token_is(tb_shader_peek_at(parser, end - parser->position), "(");
}

/* `float4(a, 1)`, `vector<float, 3>(v)`: a value built from arguments. */
static tb_shader_node *parse_construct(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_CONSTRUCT, token->location);
    tb_shader_node_list unused = {0};
    node->type = tb_shader_parse_type(parser, &unused);
    if (parser->panic || !tb_shader_expect(parser, "("))
    {
        return node;
    }
    node->children = tb_shader_parse_arguments(parser, &node->child_count);
    return finish(parser, node);
}

static tb_shader_node *parse_name_expression(tb_shader_parser *parser)
{
    const size_t begin = parser->position;
    const size_t end = tb_shader_scan_name(parser, begin);
    if (end == begin)
    {
        tb_shader_parse_expected(parser, "an expression");
        return NULL;
    }
    const tb_shader_name name = tb_shader_scanned_name(parser, begin, end);
    const tb_shader_name_kind kind = tb_shader_lookup(parser, name);
    if (kind == TB_SHADER_NAME_TYPE || kind == TB_SHADER_NAME_TEMPLATE_TYPE)
    {
        return parse_construct(parser);
    }
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_NAME, tb_shader_peek(parser)->location);
    node->name = name;
    skip_to(parser, end);
    if (kind == TB_SHADER_NAME_TEMPLATE_FUNCTION && tb_shader_accept(parser, "<"))
    {
        size_t count = 0;
        node->template_arguments = tb_shader_parse_template_arguments(parser, &count);
    }
    return finish(parser, node);
}

static tb_shader_node *parse_parenthesized(tb_shader_parser *parser)
{
    tb_shader_advance(parser);
    const size_t angle_depth = parser->angle_depth;
    parser->angle_depth = 0;
    tb_shader_node *const inner = tb_shader_parse_expression(parser);
    parser->angle_depth = angle_depth;
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ")");
    }
    return inner;
}

static tb_shader_node *parse_primary(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (token->kind == TB_SHADER_TOKEN_NUMBER)
    {
        return parse_number(parser);
    }
    if (token->kind == TB_SHADER_TOKEN_STRING)
    {
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_STRING, token->location);
        node->name = tb_shader_token_name(token);
        tb_shader_advance(parser);
        return finish(parser, node);
    }
    if (token->kind == TB_SHADER_TOKEN_CHARACTER)
    {
        tb_shader_parse_error(parser, token, "HLSL has no character constants");
        return NULL;
    }
    if (is_word(token, "true") || is_word(token, "false"))
    {
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_BOOL, token->location);
        node->integer = is_word(token, "true") ? 1u : 0u;
        tb_shader_advance(parser);
        return finish(parser, node);
    }
    if (tb_shader_is(parser, "("))
    {
        return parse_parenthesized(parser);
    }
    if (tb_shader_is_keyword(token))
    {
        tb_shader_parse_expected(parser, "an expression");
        return NULL;
    }
    return parse_name_expression(parser);
}

/* -------------------------------------------------------------------------
 * Postfix and unary expressions.
 * ------------------------------------------------------------------------- */

static tb_shader_node *parse_member(tb_shader_parser *parser, tb_shader_node *object)
{
    const tb_shader_token *const dot = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node = tb_shader_new_node(parser, TB_SHADER_NODE_MEMBER, dot->location);
    node->left = object;
    const tb_shader_token *const name = tb_shader_peek(parser);
    if (name->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        tb_shader_parse_expected(parser, "a member");
        return node;
    }
    node->name = tb_shader_token_name(name);
    tb_shader_advance(parser);
    if (at_call_template_arguments(parser))
    {
        tb_shader_advance(parser);
        size_t count = 0;
        node->template_arguments = tb_shader_parse_template_arguments(parser, &count);
    }
    return finish(parser, node);
}

static tb_shader_node *parse_index(tb_shader_parser *parser, tb_shader_node *object)
{
    const tb_shader_token *const open = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node = tb_shader_new_node(parser, TB_SHADER_NODE_INDEX, open->location);
    node->left = object;
    const size_t angle_depth = parser->angle_depth;
    parser->angle_depth = 0;
    node->right = tb_shader_parse_expression(parser);
    parser->angle_depth = angle_depth;
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, "]");
    }
    return finish(parser, node);
}

static tb_shader_node *parse_postfix(tb_shader_parser *parser)
{
    tb_shader_node *node = parse_primary(parser);
    while (!parser->panic)
    {
        const tb_shader_token *const token = tb_shader_peek(parser);
        if (tb_shader_is(parser, "["))
        {
            node = parse_index(parser, node);
        }
        else if (tb_shader_is(parser, "("))
        {
            tb_shader_advance(parser);
            tb_shader_node *const call =
                tb_shader_new_node(parser, TB_SHADER_NODE_CALL, token->location);
            call->left = node;
            call->children = tb_shader_parse_arguments(parser, &call->child_count);
            node = finish(parser, call);
        }
        else if (tb_shader_is(parser, "."))
        {
            node = parse_member(parser, node);
        }
        else if (tb_shader_is(parser, "++") || tb_shader_is(parser, "--"))
        {
            tb_shader_node *const step =
                tb_shader_new_node(parser, TB_SHADER_NODE_UNARY, token->location);
            step->op = tb_shader_is(parser, "++") ? TB_SHADER_OP_POST_INCREMENT
                                                  : TB_SHADER_OP_POST_DECREMENT;
            step->left = node;
            tb_shader_advance(parser);
            node = finish(parser, step);
        }
        else
        {
            break;
        }
    }
    return node;
}

/* Whether a `(` at the current token opens a cast: `(float3)x`. */
static bool at_cast(tb_shader_parser *parser)
{
    if (!tb_shader_is(parser, "("))
    {
        return false;
    }
    const size_t first = parser->position + 1;
    const size_t end = tb_shader_scan_type(parser, first);
    return end != first &&
           tb_shader_token_is(tb_shader_peek_at(parser, end - parser->position), ")");
}

static tb_shader_node *parse_cast(tb_shader_parser *parser)
{
    const tb_shader_token *const open = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node = tb_shader_new_node(parser, TB_SHADER_NODE_CAST, open->location);
    tb_shader_node_list unused = {0};
    node->type = tb_shader_parse_type(parser, &unused);
    if (!parser->panic && tb_shader_expect(parser, ")"))
    {
        node->left = parse_unary(parser);
    }
    return finish(parser, node);
}

static tb_shader_node *parse_sizeof(tb_shader_parser *parser)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_SIZEOF, keyword->location);
    if (at_cast(parser))
    {
        tb_shader_advance(parser);
        tb_shader_node_list unused = {0};
        node->type = tb_shader_parse_type(parser, &unused);
        (void)tb_shader_expect(parser, ")");
        return finish(parser, node);
    }
    node->left = parse_unary(parser);
    return finish(parser, node);
}

static tb_shader_node *parse_prefixed(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    const operator_spelling *const prefix =
        operator_at(parser, unary_operators, COUNT_OF(unary_operators));
    if (prefix != NULL)
    {
        tb_shader_advance(parser);
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_UNARY, token->location);
        node->op = prefix->op;
        node->left = parse_unary(parser);
        return finish(parser, node);
    }
    if (at_cast(parser))
    {
        return parse_cast(parser);
    }
    if (is_word(token, "sizeof"))
    {
        return parse_sizeof(parser);
    }
    return parse_postfix(parser);
}

static tb_shader_node *parse_unary(tb_shader_parser *parser)
{
    if (!tb_shader_enter(parser))
    {
        return NULL;
    }
    tb_shader_node *const node = parse_prefixed(parser);
    tb_shader_leave(parser);
    return node;
}

/* -------------------------------------------------------------------------
 * Binary, conditional and assignment expressions.
 * ------------------------------------------------------------------------- */

/* Operators that bind at least as tightly as `minimum`, by precedence
 * climbing: the recursion is as deep as the precedence levels, at most. */
static tb_shader_node *parse_binary(tb_shader_parser *parser, int minimum)
{
    tb_shader_node *left = parse_unary(parser);
    while (!parser->panic)
    {
        const operator_spelling *const op =
            operator_at(parser, binary_operators, COUNT_OF(binary_operators));
        if (op == NULL || op->precedence < minimum)
        {
            break;
        }
        const tb_shader_token *const token = tb_shader_peek(parser);
        tb_shader_advance(parser);
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_BINARY, token->location);
        node->op = op->op;
        node->left = left;
        node->right = parse_binary(parser, op->precedence + 1);
        left = finish(parser, node);
    }
    return left;
}

tb_shader_node *tb_shader_parse_conditional(tb_shader_parser *parser)
{
    tb_shader_node *const condition = parse_binary(parser, 1);
    if (parser->panic || !tb_shader_is(parser, "?"))
    {
        return condition;
    }
    const tb_shader_token *const question = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_CONDITIONAL, question->location);
    node->condition = condition;
    node->left = tb_shader_parse_expression(parser);
    if (!parser->panic && tb_shader_expect(parser, ":"))
    {
        node->right = tb_shader_parse_assignment(parser);
    }
    return finish(parser, node);
}

tb_shader_node *tb_shader_parse_assignment(tb_shader_parser *parser)
{
    if (!tb_shader_enter(parser))
    {
        return NULL;
    }
    tb_shader_node *node = tb_shader_parse_conditional(parser);
    const operator_spelling *const op =
        parser->panic ? NULL
                      : operator_at(parser, assignment_operators, COUNT_OF(assignment_operators));
    if (op != NULL)
    {
        const tb_shader_token *const token = tb_shader_peek(parser);
        tb_shader_advance(parser);
        tb_shader_node *const assign =
            tb_shader_new_node(parser, TB_SHADER_NODE_ASSIGN, token->location);
        assign->op = op->op;
        assign->left = node;
        assign->right = tb_shader_parse_assignment(parser);
        node = finish(parser, assign);
    }
    tb_shader_leave(parser);
    return node;
}

tb_shader_node *tb_shader_parse_expression(tb_shader_parser *parser)
{
    tb_shader_node *left = tb_shader_parse_assignment(parser);
    while (!parser->panic && tb_shader_is(parser, ","))
    {
        const tb_shader_token *const comma = tb_shader_peek(parser);
        tb_shader_advance(parser);
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_BINARY, comma->location);
        node->op = TB_SHADER_OP_COMMA;
        node->left = left;
        node->right = tb_shader_parse_assignment(parser);
        left = finish(parser, node);
    }
    return left;
}

/* -------------------------------------------------------------------------
 * Lists.
 * ------------------------------------------------------------------------- */

tb_shader_node *tb_shader_parse_initializer(tb_shader_parser *parser)
{
    if (!tb_shader_is(parser, "{"))
    {
        return tb_shader_parse_assignment(parser);
    }
    const tb_shader_token *const open = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_INITIALIZER_LIST, open->location);
    if (!tb_shader_enter(parser))
    {
        return node;
    }
    tb_shader_node_list items = {0};
    while (!tb_shader_is(parser, "}") && !tb_shader_at_end(parser) && !parser->panic)
    {
        tb_shader_node *const item = tb_shader_parse_initializer(parser);
        if (item != NULL)
        {
            tb_shader_list_append(&items, item);
        }
        if (!tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, "}");
    }
    tb_shader_leave(parser);
    node->children = items.first;
    node->child_count = items.count;
    return finish(parser, node);
}

tb_shader_node *tb_shader_parse_arguments(tb_shader_parser *parser, size_t *count)
{
    const size_t angle_depth = parser->angle_depth;
    parser->angle_depth = 0;
    tb_shader_node_list arguments = {0};
    if (!tb_shader_accept(parser, ")"))
    {
        do
        {
            tb_shader_node *const argument = tb_shader_parse_assignment(parser);
            if (parser->panic || argument == NULL)
            {
                break;
            }
            tb_shader_list_append(&arguments, argument);
        } while (tb_shader_accept(parser, ","));
        if (!parser->panic)
        {
            (void)tb_shader_expect(parser, ")");
        }
    }
    parser->angle_depth = angle_depth;
    *count = arguments.count;
    return arguments.first;
}

/* Modifiers a template argument's type may have: `RWTexture2D<unorm float4>`. */
static uint64_t parse_argument_modifiers(tb_shader_parser *parser)
{
    const uint64_t allowed = TB_SHADER_MODIFIER_SNORM | TB_SHADER_MODIFIER_UNORM |
                             TB_SHADER_MODIFIER_ROW_MAJOR | TB_SHADER_MODIFIER_COLUMN_MAJOR;
    uint64_t modifiers = 0;
    for (;;)
    {
        const uint64_t modifier = tb_shader_modifier_at(parser);
        if ((modifier & allowed) == 0)
        {
            return modifiers;
        }
        modifiers |= modifier;
        tb_shader_advance(parser);
    }
}

static tb_shader_template_argument *parse_template_argument(tb_shader_parser *parser)
{
    tb_shader_template_argument *const argument =
        TB_ARENA_NEW(parser->arena, tb_shader_template_argument);
    const uint64_t modifiers = parse_argument_modifiers(parser);
    const bool is_type = modifiers != 0 || tb_shader_is(parser, "struct") ||
                         tb_shader_scan_type(parser, parser->position) != parser->position;
    if (is_type)
    {
        tb_shader_node_list unused = {0};
        argument->type = tb_shader_parse_type(parser, &unused);
        if (argument->type != NULL)
        {
            argument->type->modifiers = modifiers;
        }
        return argument;
    }
    argument->value = tb_shader_parse_conditional(parser);
    return argument;
}

tb_shader_template_argument *tb_shader_parse_template_arguments(tb_shader_parser *parser,
                                                                size_t *count)
{
    *count = 0;
    if (!tb_shader_enter(parser))
    {
        return NULL;
    }
    parser->angle_depth++;
    tb_shader_template_argument *first = NULL;
    tb_shader_template_argument *last = NULL;
    while (!tb_shader_is(parser, ">") && !tb_shader_is(parser, ">>") && !tb_shader_at_end(parser) &&
           !parser->panic)
    {
        tb_shader_template_argument *const argument = parse_template_argument(parser);
        if (last == NULL)
        {
            first = argument;
        }
        else
        {
            last->next = argument;
        }
        last = argument;
        (*count)++;
        if (!tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    parser->angle_depth--;
    if (!parser->panic)
    {
        (void)tb_shader_expect_closing_angle(parser);
    }
    tb_shader_leave(parser);
    return first;
}
