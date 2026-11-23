/* Declarations: types, structs, interfaces and enums, functions and their
 * parameters, variables, constant buffers, typedefs, namespaces, templates,
 * attributes, modifiers, and the annotations after a `:`. */
#include "shader_parse_internal.h"

#include "badger/str.h"

#include <string.h>

/* Where a declaration is, which decides what it may declare. */
typedef enum context
{
    CONTEXT_NAMESPACE, /* At the top, or in a namespace. */
    CONTEXT_MEMBER,    /* In a struct or an interface. */
    CONTEXT_BUFFER,    /* In a cbuffer or tbuffer. */
    CONTEXT_LOCAL      /* In a function. */
} context;

static void parse_declaration_in(tb_shader_parser *parser, tb_shader_node_list *list,
                                 tb_shader_attribute *attributes, context where);

/* -------------------------------------------------------------------------
 * Modifiers.
 * ------------------------------------------------------------------------- */

typedef struct modifier_spelling
{
    const char *text;
    uint64_t bit;
} modifier_spelling;

/* Keywords: always modifiers. */
static const modifier_spelling modifier_keywords[] = {
    {"static", TB_SHADER_MODIFIER_STATIC},
    {"const", TB_SHADER_MODIFIER_CONST},
    {"uniform", TB_SHADER_MODIFIER_UNIFORM},
    {"extern", TB_SHADER_MODIFIER_EXTERN},
    {"volatile", TB_SHADER_MODIFIER_VOLATILE},
    {"groupshared", TB_SHADER_MODIFIER_GROUPSHARED},
    {"precise", TB_SHADER_MODIFIER_PRECISE},
    {"shared", TB_SHADER_MODIFIER_SHARED},
    {"inline", TB_SHADER_MODIFIER_INLINE},
    {"export", TB_SHADER_MODIFIER_EXPORT},
    {"row_major", TB_SHADER_MODIFIER_ROW_MAJOR},
    {"column_major", TB_SHADER_MODIFIER_COLUMN_MAJOR},
    {"snorm", TB_SHADER_MODIFIER_SNORM},
    {"unorm", TB_SHADER_MODIFIER_UNORM},
    {"in", TB_SHADER_MODIFIER_IN},
    {"out", TB_SHADER_MODIFIER_OUT},
    {"inout", TB_SHADER_MODIFIER_INOUT},
    {"nointerpolation", TB_SHADER_MODIFIER_NOINTERPOLATION},
    {"globallycoherent", TB_SHADER_MODIFIER_GLOBALLYCOHERENT},
};

/* Ordinary names, which are modifiers only before a declaration's type. */
static const modifier_spelling modifier_words[] = {
    {"linear", TB_SHADER_MODIFIER_LINEAR},
    {"centroid", TB_SHADER_MODIFIER_CENTROID},
    {"noperspective", TB_SHADER_MODIFIER_NOPERSPECTIVE},
    {"sample", TB_SHADER_MODIFIER_SAMPLE},
    {"point", TB_SHADER_MODIFIER_POINT},
    {"line", TB_SHADER_MODIFIER_LINE},
    {"triangle", TB_SHADER_MODIFIER_TRIANGLE},
    {"lineadj", TB_SHADER_MODIFIER_LINEADJ},
    {"triangleadj", TB_SHADER_MODIFIER_TRIANGLEADJ},
    {"indices", TB_SHADER_MODIFIER_INDICES},
    {"vertices", TB_SHADER_MODIFIER_VERTICES},
    {"primitives", TB_SHADER_MODIFIER_PRIMITIVES},
    {"payload", TB_SHADER_MODIFIER_PAYLOAD},
};

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

const char *tb_shader_modifier_name(uint64_t modifier)
{
    for (size_t index = 0; index < COUNT_OF(modifier_keywords); index++)
    {
        if (modifier_keywords[index].bit == modifier)
        {
            return modifier_keywords[index].text;
        }
    }
    for (size_t index = 0; index < COUNT_OF(modifier_words); index++)
    {
        if (modifier_words[index].bit == modifier)
        {
            return modifier_words[index].text;
        }
    }
    return NULL;
}

static uint64_t find_modifier(const tb_shader_token *token, const modifier_spelling *table,
                              size_t count)
{
    if (token->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        return 0;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (tb_shader_token_equals(token, table[index].text))
        {
            return table[index].bit;
        }
    }
    return 0;
}

/* Whether a modifier word at `index` is followed by what a declaration's
 * modifiers and type may be, so that it is itself a modifier. */
static bool word_is_modifier(tb_shader_parser *parser, size_t index)
{
    size_t cursor = index + 1;
    for (;;)
    {
        const tb_shader_token *const next = tb_shader_peek_at(parser, cursor - parser->position);
        if (find_modifier(next, modifier_keywords, COUNT_OF(modifier_keywords)) != 0)
        {
            return true;
        }
        if (find_modifier(next, modifier_words, COUNT_OF(modifier_words)) == 0)
        {
            return tb_shader_scan_type(parser, cursor) != cursor;
        }
        cursor++;
    }
}

uint64_t tb_shader_modifier_at(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    const uint64_t keyword = find_modifier(token, modifier_keywords, COUNT_OF(modifier_keywords));
    if (keyword != 0)
    {
        return keyword;
    }
    const uint64_t word = find_modifier(token, modifier_words, COUNT_OF(modifier_words));
    if (word != 0 && word_is_modifier(parser, parser->position))
    {
        return word;
    }
    return 0;
}

static uint64_t parse_modifiers(tb_shader_parser *parser)
{
    uint64_t modifiers = 0;
    for (;;)
    {
        const uint64_t modifier = tb_shader_modifier_at(parser);
        if (modifier == 0)
        {
            return modifiers;
        }
        modifiers |= modifier;
        tb_shader_advance(parser);
    }
}

/* -------------------------------------------------------------------------
 * Telling a declaration from an expression.
 * ------------------------------------------------------------------------- */

static bool is_word(const tb_shader_token *token, const char *word)
{
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && tb_shader_token_equals(token, word);
}

static bool is_declaration_keyword(const tb_shader_token *token)
{
    static const char *const words[] = {"struct",  "class",   "interface", "enum",    "typedef",
                                        "cbuffer", "tbuffer", "namespace", "template"};
    for (size_t index = 0; index < COUNT_OF(words); index++)
    {
        if (is_word(token, words[index]))
        {
            return true;
        }
    }
    return false;
}

static bool is_name(const tb_shader_token *token)
{
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && !tb_shader_is_keyword(token);
}

bool tb_shader_starts_declaration(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (is_declaration_keyword(token) || tb_shader_modifier_at(parser) != 0)
    {
        return true;
    }
    const size_t end = tb_shader_scan_type(parser, parser->position);
    if (end == parser->position)
    {
        return false;
    }
    return is_name(tb_shader_peek_at(parser, end - parser->position));
}

/* -------------------------------------------------------------------------
 * Attributes and annotations.
 * ------------------------------------------------------------------------- */

static void skip_to(tb_shader_parser *parser, size_t index)
{
    while (parser->position < index && !tb_shader_at_end(parser))
    {
        tb_shader_advance(parser);
    }
}

/* A name, possibly qualified, which the current token starts; an empty name
 * after reporting `what` when it does not start one. */
static tb_shader_name parse_name(tb_shader_parser *parser, const char *what)
{
    const size_t begin = parser->position;
    const size_t end = tb_shader_scan_name(parser, begin);
    if (end == begin)
    {
        tb_shader_parse_expected(parser, what);
        const tb_shader_name empty = {"", 0};
        return empty;
    }
    const tb_shader_name name = tb_shader_scanned_name(parser, begin, end);
    skip_to(parser, end);
    return name;
}

static tb_shader_attribute *parse_attribute(tb_shader_parser *parser)
{
    const tb_shader_token *const open = tb_shader_peek(parser);
    tb_shader_advance(parser);
    const bool doubled = tb_shader_accept(parser, "[");
    tb_shader_attribute *const attribute = TB_ARENA_NEW(parser->arena, tb_shader_attribute);
    attribute->location = open->location;
    const tb_shader_token *const name = tb_shader_peek(parser);
    if (name->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        tb_shader_parse_expected(parser, "an attribute");
        return attribute;
    }
    attribute->name = parse_name(parser, "an attribute");
    if (parser->panic)
    {
        return attribute;
    }
    if (tb_shader_accept(parser, "("))
    {
        attribute->arguments = tb_shader_parse_arguments(parser, &attribute->argument_count);
    }
    if (tb_shader_expect(parser, "]") && doubled)
    {
        (void)tb_shader_expect(parser, "]");
    }
    return attribute;
}

tb_shader_attribute *tb_shader_parse_attributes(tb_shader_parser *parser)
{
    tb_shader_attribute *first = NULL;
    tb_shader_attribute *last = NULL;
    while (tb_shader_is(parser, "[") && !parser->panic)
    {
        tb_shader_attribute *const attribute = parse_attribute(parser);
        if (last == NULL)
        {
            first = attribute;
        }
        else
        {
            last->next = attribute;
        }
        last = attribute;
    }
    return first;
}

/* The bit width of a bit field: `uint flags : 4`. */
static bool at_bit_width(tb_shader_parser *parser)
{
    return tb_shader_is(parser, ":") &&
           tb_shader_peek_at(parser, 1)->kind == TB_SHADER_TOKEN_NUMBER;
}

static tb_shader_annotation *parse_annotations(tb_shader_parser *parser)
{
    tb_shader_annotation *first = NULL;
    tb_shader_annotation *last = NULL;
    while (tb_shader_is(parser, ":") && !at_bit_width(parser) && !parser->panic)
    {
        tb_shader_advance(parser);
        const tb_shader_token *const token = tb_shader_peek(parser);
        if (token->kind != TB_SHADER_TOKEN_IDENTIFIER)
        {
            tb_shader_parse_expected(parser, "a semantic");
            break;
        }
        tb_shader_annotation *const annotation = TB_ARENA_NEW(parser->arena, tb_shader_annotation);
        annotation->location = token->location;
        annotation->name = tb_shader_token_name(token);
        tb_shader_advance(parser);
        if (tb_shader_accept(parser, "("))
        {
            annotation->has_arguments = true;
            annotation->arguments = tb_shader_parse_arguments(parser, &annotation->argument_count);
        }
        if (last == NULL)
        {
            first = annotation;
        }
        else
        {
            last->next = annotation;
        }
        last = annotation;
    }
    return first;
}

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

static tb_shader_node *parse_struct_body(tb_shader_parser *parser, tb_shader_node *node);
static tb_shader_node *parse_enum_body(tb_shader_parser *parser, tb_shader_node *node);

/* `struct`, `class`, `interface` or `enum`: a definition, or a reference to one. */
static tb_shader_type_syntax *parse_tagged_type(tb_shader_parser *parser, tb_shader_node_list *definitions)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    const bool is_enum = is_word(keyword, "enum");
    const bool is_interface = is_word(keyword, "interface");
    tb_shader_advance(parser);
    bool is_scoped = false;
    if (is_enum)
    {
        is_scoped = tb_shader_accept(parser, "class") || tb_shader_accept(parser, "struct");
    }
    tb_shader_attribute *const attributes = tb_shader_parse_attributes(parser);
    tb_shader_type_syntax *const type =
        tb_shader_new_type_syntax(parser, TB_SHADER_TYPE_SYNTAX_NAMED, keyword->location);
    type->name.text = "";
    if (is_name(tb_shader_peek(parser)))
    {
        type->name = tb_shader_token_name(tb_shader_peek(parser));
        tb_shader_advance(parser);
    }
    tb_shader_type_syntax *base = NULL;
    if (tb_shader_accept(parser, ":"))
    {
        base = tb_shader_parse_type(parser, definitions);
    }
    if (!tb_shader_is(parser, "{"))
    {
        if (type->name.length == 0)
        {
            tb_shader_parse_expected(parser, "a name");
            return NULL;
        }
        if (tb_shader_lookup(parser, type->name) == TB_SHADER_NAME_UNKNOWN)
        {
            tb_shader_declare(parser, type->name, TB_SHADER_NAME_TYPE);
        }
        return type;
    }
    tb_shader_node_kind kind = TB_SHADER_NODE_STRUCT;
    if (is_enum)
    {
        kind = TB_SHADER_NODE_ENUM;
    }
    else if (is_interface)
    {
        kind = TB_SHADER_NODE_INTERFACE;
    }
    tb_shader_node *const node = tb_shader_new_node(parser, kind, keyword->location);
    node->name = type->name;
    node->type = base;
    node->attributes = attributes;
    node->is_scoped = is_scoped;
    tb_shader_declare(parser, node->name, TB_SHADER_NAME_TYPE);
    if (is_enum)
    {
        (void)parse_enum_body(parser, node);
    }
    else
    {
        (void)parse_struct_body(parser, node);
    }
    tb_shader_list_append(definitions, node);
    type->definition = node;
    return type;
}

tb_shader_type_syntax *tb_shader_parse_type(tb_shader_parser *parser, tb_shader_node_list *definitions)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (is_word(token, "struct") || is_word(token, "class") || is_word(token, "interface") ||
        is_word(token, "enum"))
    {
        return parse_tagged_type(parser, definitions);
    }
    const size_t begin = parser->position;
    const size_t end = tb_shader_scan_name(parser, begin);
    if (end == begin)
    {
        tb_shader_parse_expected(parser, "a type");
        return NULL;
    }
    const tb_shader_name name = tb_shader_scanned_name(parser, begin, end);
    const tb_shader_name_kind kind = tb_shader_lookup(parser, name);
    if (kind != TB_SHADER_NAME_TYPE && kind != TB_SHADER_NAME_TEMPLATE_TYPE)
    {
        const int length = (int)name.length;
        if (kind == TB_SHADER_NAME_UNKNOWN)
        {
            tb_shader_parse_error(parser, token, "unknown type '%.*s'", length, name.text);
        }
        else
        {
            tb_shader_parse_error(parser, token, "'%.*s' is not a type", length, name.text);
        }
        return NULL;
    }
    skip_to(parser, end);
    tb_shader_type_syntax *const type = tb_shader_new_type_syntax(parser, TB_SHADER_TYPE_SYNTAX_NAMED, token->location);
    type->name = name;
    if (kind == TB_SHADER_NAME_TEMPLATE_TYPE && tb_shader_accept(parser, "<"))
    {
        type->has_arguments = true;
        type->arguments = tb_shader_parse_template_arguments(parser, &type->argument_count);
    }
    return type;
}

/* `[4]`, `[]`, `[2][3]` after a declarator's name, around `element`. */
static tb_shader_type_syntax *parse_array_dimensions(tb_shader_parser *parser, tb_shader_type_syntax *element)
{
    tb_shader_type_syntax *outer = element;
    tb_shader_type_syntax **slot = &outer;
    while (tb_shader_is(parser, "[") && !parser->panic)
    {
        const tb_shader_token *const open = tb_shader_peek(parser);
        tb_shader_advance(parser);
        tb_shader_type_syntax *const array =
            tb_shader_new_type_syntax(parser, TB_SHADER_TYPE_SYNTAX_ARRAY, open->location);
        if (!tb_shader_is(parser, "]"))
        {
            array->size = tb_shader_parse_conditional(parser);
        }
        (void)tb_shader_expect(parser, "]");
        array->element = element;
        *slot = array;
        slot = &array->element;
    }
    return outer;
}

/* -------------------------------------------------------------------------
 * Structs, interfaces and enums.
 * ------------------------------------------------------------------------- */

static bool starts_member(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    return token->kind == TB_SHADER_TOKEN_PRAGMA || is_declaration_keyword(token) ||
           tb_shader_modifier_at(parser) != 0 ||
           tb_shader_scan_type(parser, parser->position) != parser->position;
}

static tb_shader_node *parse_struct_body(tb_shader_parser *parser, tb_shader_node *node)
{
    (void)tb_shader_expect(parser, "{");
    if (!tb_shader_enter(parser))
    {
        return node;
    }
    /* A struct is a scope of its own: a member is `S::member` outside it.
     * Inside a function, and for a struct without a name, its members are
     * local names instead. */
    const bool is_local = parser->local_depth > 0 || node->name.length == 0;
    const size_t mark = tb_shader_push_scope(parser);
    tb_shader_name outer = parser->path;
    if (is_local)
    {
        parser->local_depth++;
    }
    else
    {
        outer = tb_shader_enter_path(parser, node->name);
    }
    tb_shader_node_list members = {0};
    while (!tb_shader_at_end(parser) && !tb_shader_is(parser, "}"))
    {
        const size_t before = parser->position;
        if (tb_shader_accept(parser, ";"))
        {
            continue;
        }
        tb_shader_attribute *const attributes = tb_shader_parse_attributes(parser);
        if (!parser->panic && starts_member(parser))
        {
            parse_declaration_in(parser, &members, attributes, CONTEXT_MEMBER);
        }
        else
        {
            tb_shader_parse_expected(parser, "a member");
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
    if (is_local)
    {
        parser->local_depth--;
    }
    tb_shader_pop_scope(parser, mark);
    parser->path = outer;
    tb_shader_leave(parser);
    node->children = members.first;
    node->child_count = members.count;
    return node;
}

static tb_shader_node *parse_enumerator(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (!is_name(token))
    {
        tb_shader_parse_expected(parser, "an enumerator");
        return NULL;
    }
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_ENUMERATOR, token->location);
    node->name = tb_shader_token_name(token);
    tb_shader_advance(parser);
    if (tb_shader_accept(parser, "="))
    {
        node->value = tb_shader_parse_conditional(parser);
    }
    return node;
}

static tb_shader_node *parse_enum_body(tb_shader_parser *parser, tb_shader_node *node)
{
    (void)tb_shader_expect(parser, "{");
    /* The enumerators of `enum class E` are `E::X`; the others are names of
     * the scope the enum is in. */
    tb_shader_name outer = parser->path;
    const bool enters = node->is_scoped && parser->local_depth == 0;
    if (enters)
    {
        outer = tb_shader_enter_path(parser, node->name);
    }
    tb_shader_node_list enumerators = {0};
    while (!tb_shader_at_end(parser) && !tb_shader_is(parser, "}") && !parser->panic)
    {
        tb_shader_node *const enumerator = parse_enumerator(parser);
        if (enumerator == NULL)
        {
            break;
        }
        tb_shader_declare(parser, enumerator->name, TB_SHADER_NAME_VALUE);
        tb_shader_list_append(&enumerators, enumerator);
        if (!tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    parser->path = outer;
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, "}");
    }
    node->children = enumerators.first;
    node->child_count = enumerators.count;
    return node;
}

/* -------------------------------------------------------------------------
 * Functions.
 * ------------------------------------------------------------------------- */

static tb_shader_node *parse_parameter(tb_shader_parser *parser)
{
    const tb_shader_token *const first = tb_shader_peek(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_PARAMETER, first->location);
    node->name.text = "";
    node->attributes = tb_shader_parse_attributes(parser);
    node->modifiers = parse_modifiers(parser);
    tb_shader_node_list definitions = {0};
    tb_shader_type_syntax *const type = tb_shader_parse_type(parser, &definitions);
    if (type == NULL)
    {
        return node;
    }
    if (definitions.count > 0)
    {
        tb_shader_parse_error(parser, first, "a parameter cannot define a type");
    }
    if (is_name(tb_shader_peek(parser)))
    {
        node->name = tb_shader_token_name(tb_shader_peek(parser));
        node->location = tb_shader_peek(parser)->location;
        tb_shader_advance(parser);
    }
    node->type = parse_array_dimensions(parser, type);
    node->annotations = parse_annotations(parser);
    if (tb_shader_accept(parser, "="))
    {
        node->value = tb_shader_parse_assignment(parser);
    }
    tb_shader_declare(parser, node->name, TB_SHADER_NAME_VALUE);
    return node;
}

static void parse_parameters(tb_shader_parser *parser, tb_shader_node *function)
{
    tb_shader_node_list parameters = {0};
    const bool is_void = is_word(tb_shader_peek(parser), "void") &&
                         tb_shader_token_is(tb_shader_peek_at(parser, 1), ")");
    if (is_void)
    {
        tb_shader_advance(parser);
    }
    while (!tb_shader_is(parser, ")") && !tb_shader_at_end(parser) && !parser->panic)
    {
        tb_shader_list_append(&parameters, parse_parameter(parser));
        if (!tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    (void)tb_shader_expect(parser, ")");
    function->parameters = parameters.first;
    function->parameter_count = parameters.count;
}

/* The part of a qualified name before its last `::`: `S` of `S::f`, or empty. */
static tb_shader_name qualifier_of(tb_shader_name name)
{
    size_t length = name.length;
    while (length >= 2 && !(name.text[length - 2] == ':' && name.text[length - 1] == ':'))
    {
        length--;
    }
    const tb_shader_name qualifier = {name.text, length >= 2 ? length - 2 : 0};
    return qualifier;
}

/* A function, after its name, at its `(`. */
static tb_shader_node *parse_function(tb_shader_parser *parser, tb_shader_node *node)
{
    node->kind = TB_SHADER_NODE_FUNCTION;
    tb_shader_declare(parser, node->name, TB_SHADER_NAME_VALUE);
    tb_shader_advance(parser);
    /* `float Light::Intensity() { ... }` reads Light's members by their own names. */
    const tb_shader_name qualifier = qualifier_of(node->name);
    const tb_shader_name outer = parser->path;
    if (qualifier.length > 0 && parser->local_depth == 0)
    {
        (void)tb_shader_enter_path(parser, qualifier);
    }
    const size_t mark = tb_shader_push_scope(parser);
    parser->local_depth++;
    parse_parameters(parser, node);
    node->annotations = parse_annotations(parser);
    if (tb_shader_is(parser, "{"))
    {
        node->body = tb_shader_parse_block(parser);
    }
    else if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
    parser->local_depth--;
    tb_shader_pop_scope(parser, mark);
    parser->path = outer;
    return node;
}

/* -------------------------------------------------------------------------
 * Declarators.
 * ------------------------------------------------------------------------- */

/* `operator+`, `operator[]`, `operator()`: the name of an overloaded operator. */
static tb_shader_name parse_operator_name(tb_shader_parser *parser)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    tb_shader_advance(parser);
    const tb_shader_token *const op = tb_shader_peek(parser);
    if (op->kind != TB_SHADER_TOKEN_PUNCTUATOR)
    {
        tb_shader_parse_expected(parser, "an operator");
        return tb_shader_token_name(keyword);
    }
    tb_shader_name name =
        tb_shader_join_names(parser, tb_shader_token_name(keyword), "", tb_shader_token_name(op));
    const bool is_pair = tb_shader_token_is(op, "(") || tb_shader_token_is(op, "[");
    tb_shader_advance(parser);
    if (is_pair)
    {
        const tb_shader_token *const close = tb_shader_peek(parser);
        if (tb_shader_expect(parser, tb_shader_token_is(op, "(") ? ")" : "]"))
        {
            name = tb_shader_join_names(parser, name, "", tb_shader_token_name(close));
        }
    }
    return name;
}

static tb_shader_name parse_declarator_name(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (is_word(token, "operator") &&
        tb_shader_peek_at(parser, 1)->kind == TB_SHADER_TOKEN_PUNCTUATOR)
    {
        return parse_operator_name(parser);
    }
    return parse_name(parser, "a name");
}

/* A variable, after its name. */
static tb_shader_node *parse_variable(tb_shader_parser *parser, tb_shader_node *node, context where)
{
    node->kind = TB_SHADER_NODE_VARIABLE;
    node->type = parse_array_dimensions(parser, node->type);
    node->annotations = parse_annotations(parser);
    if (where == CONTEXT_MEMBER && at_bit_width(parser))
    {
        tb_shader_advance(parser);
        node->is_bit_field = true;
        node->value = tb_shader_parse_conditional(parser);
    }
    tb_shader_declare(parser, node->name, TB_SHADER_NAME_VALUE);
    if (tb_shader_accept(parser, "="))
    {
        node->value = tb_shader_parse_initializer(parser);
    }
    return node;
}

/* The declarators after a declaration's type: `a = 1, b[2];`, or a function. */
static void parse_declarators(tb_shader_parser *parser, tb_shader_node_list *list,
                              tb_shader_node *pattern, context where)
{
    for (size_t index = 0;; index++)
    {
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_VARIABLE, tb_shader_peek(parser)->location);
        *node = *pattern;
        node->location = tb_shader_peek(parser)->location;
        node->name = parse_declarator_name(parser);
        if (parser->panic)
        {
            return;
        }
        if (index == 0 && tb_shader_is(parser, "(") && where != CONTEXT_BUFFER)
        {
            tb_shader_list_append(list, parse_function(parser, node));
            return;
        }
        tb_shader_list_append(list, parse_variable(parser, node, where));
        if (parser->panic || !tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
}

/* -------------------------------------------------------------------------
 * Typedefs, buffers, namespaces and templates.
 * ------------------------------------------------------------------------- */

static void parse_typedef(tb_shader_parser *parser, tb_shader_node_list *list)
{
    tb_shader_advance(parser);
    const uint64_t modifiers = parse_modifiers(parser);
    tb_shader_type_syntax *const type = tb_shader_parse_type(parser, list);
    if (type == NULL)
    {
        return;
    }
    do
    {
        const tb_shader_token *const token = tb_shader_peek(parser);
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_TYPEDEF, token->location);
        node->modifiers = modifiers;
        node->name = parse_name(parser, "a name");
        if (parser->panic)
        {
            return;
        }
        node->type = parse_array_dimensions(parser, type);
        tb_shader_declare(parser, node->name, TB_SHADER_NAME_TYPE);
        tb_shader_list_append(list, node);
    } while (tb_shader_accept(parser, ",") && !parser->panic);
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ";");
    }
}

static void parse_buffer(tb_shader_parser *parser, tb_shader_node_list *list,
                         tb_shader_attribute *attributes)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    const tb_shader_node_kind kind =
        is_word(keyword, "cbuffer") ? TB_SHADER_NODE_CBUFFER : TB_SHADER_NODE_TBUFFER;
    tb_shader_advance(parser);
    tb_shader_node *const node = tb_shader_new_node(parser, kind, keyword->location);
    node->attributes = attributes;
    node->name = parse_name(parser, "a name");
    node->annotations = parse_annotations(parser);
    if (parser->panic || !tb_shader_expect(parser, "{"))
    {
        return;
    }
    /* A buffer's members are names of the scope the buffer is in. */
    tb_shader_node_list members = {0};
    while (!tb_shader_at_end(parser) && !tb_shader_is(parser, "}"))
    {
        const size_t before = parser->position;
        if (tb_shader_accept(parser, ";"))
        {
            continue;
        }
        tb_shader_attribute *const member_attributes = tb_shader_parse_attributes(parser);
        if (!parser->panic && starts_member(parser))
        {
            parse_declaration_in(parser, &members, member_attributes, CONTEXT_BUFFER);
        }
        else
        {
            tb_shader_parse_expected(parser, "a member");
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
    (void)tb_shader_accept(parser, ";");
    node->children = members.first;
    node->child_count = members.count;
    tb_shader_list_append(list, node);
}

static void parse_namespace(tb_shader_parser *parser, tb_shader_node_list *list)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    tb_shader_advance(parser);
    tb_shader_node *const node =
        tb_shader_new_node(parser, TB_SHADER_NODE_NAMESPACE, keyword->location);
    node->name = parse_name(parser, "a name");
    if (parser->panic || !tb_shader_expect(parser, "{") || !tb_shader_enter(parser))
    {
        return;
    }
    tb_shader_declare(parser, node->name, TB_SHADER_NAME_NAMESPACE);
    const tb_shader_name outer = tb_shader_enter_path(parser, node->name);
    tb_shader_node_list declarations = {0};
    tb_shader_parse_declarations(parser, &declarations, true);
    (void)tb_shader_expect(parser, "}");
    parser->path = outer;
    tb_shader_leave(parser);
    node->children = declarations.first;
    node->child_count = declarations.count;
    tb_shader_list_append(list, node);
}

static tb_shader_node *parse_template_parameter(tb_shader_parser *parser)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (is_word(token, "typename") || is_word(token, "class"))
    {
        tb_shader_advance(parser);
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_TEMPLATE_PARAMETER, token->location);
        node->name = parse_name(parser, "a name");
        if (tb_shader_accept(parser, "="))
        {
            tb_shader_node_list definitions = {0};
            node->type = tb_shader_parse_type(parser, &definitions);
        }
        tb_shader_declare_scoped(parser, node->name, TB_SHADER_NAME_TYPE);
        return node;
    }
    /* A value parameter is a name of the template's scope, not of the namespace's. */
    parser->local_depth++;
    tb_shader_node *const node = parse_parameter(parser);
    parser->local_depth--;
    return node;
}

static void parse_template(tb_shader_parser *parser, tb_shader_node_list *list,
                           tb_shader_attribute *attributes, context where)
{
    const tb_shader_token *const keyword = tb_shader_peek(parser);
    tb_shader_advance(parser);
    if (!tb_shader_expect(parser, "<"))
    {
        return;
    }
    const size_t mark = tb_shader_push_scope(parser);
    tb_shader_node_list parameters = {0};
    while (!tb_shader_is(parser, ">") && !tb_shader_at_end(parser) && !parser->panic)
    {
        tb_shader_list_append(&parameters, parse_template_parameter(parser));
        if (!tb_shader_accept(parser, ","))
        {
            break;
        }
    }
    if (!parser->panic)
    {
        (void)tb_shader_expect(parser, ">");
    }
    tb_shader_node_list declared = {0};
    if (!parser->panic)
    {
        parse_declaration_in(parser, &declared, attributes, where);
    }
    tb_shader_pop_scope(parser, mark);
    /* What a template declares is its struct or its function. */
    tb_shader_node *templated = NULL;
    for (tb_shader_node *node = declared.first; node != NULL; node = node->next)
    {
        if (templated == NULL &&
            (node->kind == TB_SHADER_NODE_STRUCT || node->kind == TB_SHADER_NODE_FUNCTION))
        {
            templated = node;
        }
    }
    if (templated != NULL)
    {
        templated->is_template = true;
        templated->template_parameters = parameters.first;
        tb_shader_declare(parser, templated->name,
                          templated->kind == TB_SHADER_NODE_STRUCT
                              ? TB_SHADER_NAME_TEMPLATE_TYPE
                              : TB_SHADER_NAME_TEMPLATE_FUNCTION);
    }
    else if (!parser->panic && declared.count > 0)
    {
        tb_shader_parse_error(parser, keyword, "a template declares a struct or a function");
    }
    for (tb_shader_node *node = declared.first; node != NULL;)
    {
        tb_shader_node *const next = node->next;
        node->next = NULL;
        tb_shader_list_append(list, node);
        node = next;
    }
}

/* -------------------------------------------------------------------------
 * Declarations.
 * ------------------------------------------------------------------------- */

static bool allowed_in(tb_shader_parser *parser, const tb_shader_token *keyword, context where)
{
    if (is_word(keyword, "namespace") || is_word(keyword, "cbuffer") || is_word(keyword, "tbuffer"))
    {
        if (where == CONTEXT_NAMESPACE)
        {
            return true;
        }
        tb_shader_parse_error(parser, keyword, "'%.*s' is allowed only at namespace scope",
                              (int)keyword->length, keyword->text);
        return false;
    }
    if (is_word(keyword, "template") && where != CONTEXT_NAMESPACE && where != CONTEXT_MEMBER)
    {
        tb_shader_parse_error(parser, keyword, "a template cannot be declared here");
        return false;
    }
    return true;
}

static void parse_declaration_in(tb_shader_parser *parser, tb_shader_node_list *list,
                                 tb_shader_attribute *attributes, context where)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (!tb_shader_enter(parser))
    {
        return;
    }
    if (token->kind == TB_SHADER_TOKEN_PRAGMA)
    {
        tb_shader_node *const node =
            tb_shader_new_node(parser, TB_SHADER_NODE_PRAGMA, token->location);
        node->name = tb_shader_token_name(token);
        tb_shader_advance(parser);
        tb_shader_list_append(list, node);
    }
    else if (!allowed_in(parser, token, where))
    {
        /* Reported. */
    }
    else if (is_word(token, "namespace"))
    {
        parse_namespace(parser, list);
    }
    else if (is_word(token, "cbuffer") || is_word(token, "tbuffer"))
    {
        parse_buffer(parser, list, attributes);
    }
    else if (is_word(token, "typedef"))
    {
        parse_typedef(parser, list);
    }
    else if (is_word(token, "template"))
    {
        parse_template(parser, list, attributes, where);
    }
    else if (token->kind != TB_SHADER_TOKEN_IDENTIFIER && !tb_shader_token_is(token, "::"))
    {
        tb_shader_parse_expected(parser, "a declaration");
    }
    else
    {
        tb_shader_node pattern = {0};
        pattern.location = token->location;
        pattern.attributes = attributes;
        pattern.modifiers = parse_modifiers(parser);
        const size_t definitions_before = list->count;
        pattern.type = tb_shader_parse_type(parser, list);
        if (pattern.type != NULL && !parser->panic)
        {
            const bool defines_type = list->count > definitions_before;
            if (!defines_type || !tb_shader_accept(parser, ";"))
            {
                parse_declarators(parser, list, &pattern, where);
            }
        }
    }
    tb_shader_leave(parser);
}

void tb_shader_parse_declaration(tb_shader_parser *parser, tb_shader_node_list *list,
                                 tb_shader_attribute *attributes)
{
    parse_declaration_in(parser, list, attributes, CONTEXT_LOCAL);
}

void tb_shader_parse_declarations(tb_shader_parser *parser, tb_shader_node_list *list,
                                  bool in_braces)
{
    while (!tb_shader_at_end(parser))
    {
        if (in_braces && tb_shader_is(parser, "}"))
        {
            return;
        }
        const size_t before = parser->position;
        if (tb_shader_accept(parser, ";"))
        {
            continue;
        }
        if (tb_shader_is(parser, "}"))
        {
            tb_shader_parse_error(parser, tb_shader_peek(parser), "unexpected '}'");
            tb_shader_advance(parser);
            parser->panic = false;
            continue;
        }
        tb_shader_attribute *const attributes = tb_shader_parse_attributes(parser);
        if (!parser->panic)
        {
            parse_declaration_in(parser, list, attributes, CONTEXT_NAMESPACE);
        }
        if (parser->panic)
        {
            tb_shader_recover(parser);
        }
        if (parser->position == before && !tb_shader_at_end(parser))
        {
            tb_shader_advance(parser);
        }
    }
}
