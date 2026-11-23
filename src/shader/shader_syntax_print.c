/* A syntax tree as text: one S-expression for each declaration, so tests can
 * state the tree a source makes, and `badger-shaderc` can show it. */
#include "shader_syntax.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/str.h"

#include <string.h>

typedef struct printer
{
    tb_array *text;
} printer;

static void print_node(printer *out, const tb_shader_node *node);
static void print_type(printer *out, const tb_shader_type *type);

static void put(printer *out, const char *text)
{
    tb_array_append(out->text, text, tb_str_length(text));
}

static void put_name(printer *out, tb_shader_name name)
{
    tb_array_append(out->text, name.text, name.length);
}

static void put_node_list(printer *out, const tb_shader_node *first, const char *separator)
{
    for (const tb_shader_node *node = first; node != NULL; node = node->next)
    {
        print_node(out, node);
        if (node->next != NULL)
        {
            put(out, separator);
        }
    }
}

/* ` a b c`: each node of a list after a space. */
static void put_children(printer *out, const tb_shader_node *first)
{
    for (const tb_shader_node *node = first; node != NULL; node = node->next)
    {
        put(out, " ");
        print_node(out, node);
    }
}

static void put_modifiers(printer *out, uint64_t modifiers)
{
    for (unsigned bit = 0; bit < 64; bit++)
    {
        const uint64_t modifier = UINT64_C(1) << bit;
        if ((modifiers & modifier) != 0)
        {
            put(out, tb_shader_modifier_name(modifier));
            put(out, " ");
        }
    }
}

static void put_attributes(printer *out, const tb_shader_attribute *attribute)
{
    for (; attribute != NULL; attribute = attribute->next)
    {
        put(out, " [");
        put_name(out, attribute->name);
        if (attribute->arguments != NULL)
        {
            put(out, "(");
            put_node_list(out, attribute->arguments, ", ");
            put(out, ")");
        }
        put(out, "]");
    }
}

static void put_annotations(printer *out, const tb_shader_annotation *annotation)
{
    for (; annotation != NULL; annotation = annotation->next)
    {
        put(out, " :");
        put_name(out, annotation->name);
        if (annotation->has_arguments)
        {
            put(out, "(");
            put_node_list(out, annotation->arguments, ", ");
            put(out, ")");
        }
    }
}

static void put_template_arguments(printer *out, const tb_shader_template_argument *argument)
{
    put(out, "<");
    for (; argument != NULL; argument = argument->next)
    {
        if (argument->type != NULL)
        {
            print_type(out, argument->type);
        }
        else
        {
            print_node(out, argument->value);
        }
        if (argument->next != NULL)
        {
            put(out, ", ");
        }
    }
    put(out, ">");
}

static void print_type(printer *out, const tb_shader_type *type)
{
    if (type == NULL)
    {
        put(out, "<error>");
        return;
    }
    const tb_shader_type *base = type;
    while (base->kind == TB_SHADER_TYPE_ARRAY && base->element != NULL)
    {
        base = base->element;
    }
    put_modifiers(out, base->modifiers);
    if (base->name.length == 0)
    {
        put(out, "<anonymous>");
    }
    else
    {
        put_name(out, base->name);
    }
    if (base->has_arguments)
    {
        put_template_arguments(out, base->arguments);
    }
    for (const tb_shader_type *array = type; array != base; array = array->element)
    {
        put(out, "[");
        if (array->size != NULL)
        {
            print_node(out, array->size);
        }
        put(out, "]");
    }
}

/* ` type` of a declaration. */
static void put_type(printer *out, const tb_shader_type *type)
{
    put(out, " ");
    print_type(out, type);
}

static void put_optional(printer *out, const tb_shader_node *node)
{
    put(out, " ");
    if (node == NULL)
    {
        put(out, "_");
        return;
    }
    print_node(out, node);
}

static void put_template_parameters(printer *out, const tb_shader_node *node)
{
    if (!node->is_template)
    {
        return;
    }
    put(out, " template(");
    put_node_list(out, node->template_parameters, " ");
    put(out, ")");
}

/* -------------------------------------------------------------------------
 * Declarations.
 * ------------------------------------------------------------------------- */

static void print_struct(printer *out, const tb_shader_node *node, const char *keyword)
{
    put(out, keyword);
    put_template_parameters(out, node);
    put_attributes(out, node->attributes);
    if (node->name.length > 0)
    {
        put(out, " ");
        put_name(out, node->name);
    }
    if (node->type != NULL)
    {
        put(out, " :");
        put_type(out, node->type);
    }
    put_children(out, node->children);
    put(out, ")");
}

static void print_function(printer *out, const tb_shader_node *node)
{
    put(out, "(function");
    put_template_parameters(out, node);
    put_attributes(out, node->attributes);
    put(out, " ");
    put_modifiers(out, node->modifiers);
    print_type(out, node->type);
    put(out, " ");
    put_name(out, node->name);
    put(out, " (");
    put_node_list(out, node->parameters, " ");
    put(out, ")");
    put_annotations(out, node->annotations);
    if (node->body != NULL)
    {
        put(out, " ");
        print_node(out, node->body);
    }
    put(out, ")");
}

/* A variable or a parameter. */
static void print_variable(printer *out, const tb_shader_node *node, const char *keyword)
{
    put(out, keyword);
    put_attributes(out, node->attributes);
    put(out, " ");
    put_modifiers(out, node->modifiers);
    print_type(out, node->type);
    if (node->name.length > 0)
    {
        put(out, " ");
        put_name(out, node->name);
    }
    put_annotations(out, node->annotations);
    if (node->value != NULL)
    {
        put(out, node->is_bit_field ? " : " : " = ");
        print_node(out, node->value);
    }
    put(out, ")");
}

static void print_named_list(printer *out, const tb_shader_node *node, const char *keyword)
{
    put(out, keyword);
    put_attributes(out, node->attributes);
    if (node->name.length > 0)
    {
        put(out, " ");
        put_name(out, node->name);
    }
    put_annotations(out, node->annotations);
    put_children(out, node->children);
    put(out, ")");
}

static void print_enum(printer *out, const tb_shader_node *node)
{
    put(out, node->is_scoped ? "(enum class" : "(enum");
    if (node->name.length > 0)
    {
        put(out, " ");
        put_name(out, node->name);
    }
    if (node->type != NULL)
    {
        put(out, " :");
        put_type(out, node->type);
    }
    put_children(out, node->children);
    put(out, ")");
}

static void print_typedef(printer *out, const tb_shader_node *node)
{
    put(out, "(typedef ");
    put_modifiers(out, node->modifiers);
    print_type(out, node->type);
    put(out, " ");
    put_name(out, node->name);
    put(out, ")");
}

static void print_enumerator(printer *out, const tb_shader_node *node)
{
    put(out, "(");
    put_name(out, node->name);
    if (node->value != NULL)
    {
        put(out, " = ");
        print_node(out, node->value);
    }
    put(out, ")");
}

static void print_template_parameter(printer *out, const tb_shader_node *node)
{
    put(out, "(typename ");
    put_name(out, node->name);
    if (node->type != NULL)
    {
        put(out, " =");
        put_type(out, node->type);
    }
    put(out, ")");
}

static void print_pragma(printer *out, const tb_shader_node *node)
{
    put(out, "(pragma ");
    put_name(out, node->name);
    put(out, ")");
}

/* -------------------------------------------------------------------------
 * Statements.
 * ------------------------------------------------------------------------- */

/* `(keyword` and the statement's attributes. */
static void open_statement(printer *out, const tb_shader_node *node, const char *keyword)
{
    put(out, "(");
    put(out, keyword);
    put_attributes(out, node->attributes);
}

/* `(keyword [attributes] a b ...)`, with `_` for each part that is absent. */
static void print_statement(printer *out, const tb_shader_node *node, const char *keyword,
                            const tb_shader_node *const *parts, size_t part_count)
{
    open_statement(out, node, keyword);
    for (size_t index = 0; index < part_count; index++)
    {
        put_optional(out, parts[index]);
    }
    put(out, ")");
}

static void print_list_statement(printer *out, const tb_shader_node *node, const char *keyword)
{
    open_statement(out, node, keyword);
    put_children(out, node->children);
    put(out, ")");
}

static void print_if(printer *out, const tb_shader_node *node)
{
    const tb_shader_node *const parts[] = {node->condition, node->body, node->else_body};
    print_statement(out, node, "if", parts, node->else_body != NULL ? 3 : 2);
}

static void print_for(printer *out, const tb_shader_node *node)
{
    const tb_shader_node *const parts[] = {node->init, node->condition, node->step, node->body};
    print_statement(out, node, "for", parts, 4);
}

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

/* `(op a b)`. */
static void print_operation(printer *out, const char *op, const tb_shader_node *left,
                            const tb_shader_node *right)
{
    put(out, "(");
    put(out, op);
    put_optional(out, left);
    if (right != NULL)
    {
        put_optional(out, right);
    }
    put(out, ")");
}

static void print_unary(printer *out, const tb_shader_node *node)
{
    const char *op = tb_shader_operator_spelling(node->op);
    if (node->op == TB_SHADER_OP_PRE_INCREMENT)
    {
        op = "pre++";
    }
    else if (node->op == TB_SHADER_OP_PRE_DECREMENT)
    {
        op = "pre--";
    }
    else if (node->op == TB_SHADER_OP_POST_INCREMENT)
    {
        op = "post++";
    }
    else if (node->op == TB_SHADER_OP_POST_DECREMENT)
    {
        op = "post--";
    }
    print_operation(out, op, node->left, NULL);
}

static void print_name(printer *out, const tb_shader_node *node)
{
    put_name(out, node->name);
    if (node->template_arguments != NULL)
    {
        put_template_arguments(out, node->template_arguments);
    }
}

static void print_conditional(printer *out, const tb_shader_node *node)
{
    put(out, "(?");
    put_optional(out, node->condition);
    put_optional(out, node->left);
    put_optional(out, node->right);
    put(out, ")");
}

static void print_call(printer *out, const tb_shader_node *node)
{
    put(out, "(call");
    put_optional(out, node->left);
    put_children(out, node->children);
    put(out, ")");
}

static void print_construct(printer *out, const tb_shader_node *node)
{
    put(out, "(construct");
    put_type(out, node->type);
    put_children(out, node->children);
    put(out, ")");
}

static void print_cast(printer *out, const tb_shader_node *node)
{
    put(out, "(cast");
    put_type(out, node->type);
    put_optional(out, node->left);
    put(out, ")");
}

static void print_member(printer *out, const tb_shader_node *node)
{
    put(out, "(.");
    put_optional(out, node->left);
    put(out, " ");
    print_name(out, node);
    put(out, ")");
}

static void print_sizeof(printer *out, const tb_shader_node *node)
{
    put(out, "(sizeof");
    if (node->type != NULL)
    {
        put_type(out, node->type);
    }
    else
    {
        put_optional(out, node->left);
    }
    put(out, ")");
}

/* -------------------------------------------------------------------------
 * Every node.
 * ------------------------------------------------------------------------- */

static void print_by_kind(printer *out, const tb_shader_node *node)
{
    switch (node->kind)
    {
    case TB_SHADER_NODE_STRUCT:
        print_struct(out, node, "(struct");
        break;
    case TB_SHADER_NODE_INTERFACE:
        print_struct(out, node, "(interface");
        break;
    case TB_SHADER_NODE_FUNCTION:
        print_function(out, node);
        break;
    case TB_SHADER_NODE_VARIABLE:
        print_variable(out, node, "(variable");
        break;
    case TB_SHADER_NODE_PARAMETER:
        print_variable(out, node, "(parameter");
        break;
    case TB_SHADER_NODE_CBUFFER:
        print_named_list(out, node, "(cbuffer");
        break;
    case TB_SHADER_NODE_TBUFFER:
        print_named_list(out, node, "(tbuffer");
        break;
    case TB_SHADER_NODE_NAMESPACE:
        print_named_list(out, node, "(namespace");
        break;
    case TB_SHADER_NODE_TYPEDEF:
        print_typedef(out, node);
        break;
    case TB_SHADER_NODE_ENUM:
        print_enum(out, node);
        break;
    case TB_SHADER_NODE_ENUMERATOR:
        print_enumerator(out, node);
        break;
    case TB_SHADER_NODE_TEMPLATE_PARAMETER:
        print_template_parameter(out, node);
        break;
    case TB_SHADER_NODE_PRAGMA:
        print_pragma(out, node);
        break;
    case TB_SHADER_NODE_BLOCK:
        print_list_statement(out, node, "block");
        break;
    case TB_SHADER_NODE_DECLARATION:
        print_list_statement(out, node, "declare");
        break;
    case TB_SHADER_NODE_EXPRESSION_STATEMENT: {
        const tb_shader_node *const parts[] = {node->value};
        print_statement(out, node, "expr", parts, 1);
        break;
    }
    case TB_SHADER_NODE_IF:
        print_if(out, node);
        break;
    case TB_SHADER_NODE_FOR:
        print_for(out, node);
        break;
    case TB_SHADER_NODE_WHILE: {
        const tb_shader_node *const parts[] = {node->condition, node->body};
        print_statement(out, node, "while", parts, 2);
        break;
    }
    case TB_SHADER_NODE_DO: {
        const tb_shader_node *const parts[] = {node->body, node->condition};
        print_statement(out, node, "do-while", parts, 2);
        break;
    }
    case TB_SHADER_NODE_SWITCH: {
        const tb_shader_node *const parts[] = {node->condition, node->body};
        print_statement(out, node, "switch", parts, 2);
        break;
    }
    case TB_SHADER_NODE_CASE: {
        const tb_shader_node *const parts[] = {node->value};
        print_statement(out, node, "case", parts, 1);
        break;
    }
    case TB_SHADER_NODE_RETURN: {
        const tb_shader_node *const parts[] = {node->value};
        print_statement(out, node, "return", parts, node->value != NULL ? 1 : 0);
        break;
    }
    case TB_SHADER_NODE_DEFAULT:
        print_statement(out, node, "default", NULL, 0);
        break;
    case TB_SHADER_NODE_BREAK:
        print_statement(out, node, "break", NULL, 0);
        break;
    case TB_SHADER_NODE_CONTINUE:
        print_statement(out, node, "continue", NULL, 0);
        break;
    case TB_SHADER_NODE_DISCARD:
        print_statement(out, node, "discard", NULL, 0);
        break;
    case TB_SHADER_NODE_EMPTY:
        print_statement(out, node, "empty", NULL, 0);
        break;
    case TB_SHADER_NODE_INTEGER:
    case TB_SHADER_NODE_FLOAT:
    case TB_SHADER_NODE_STRING:
        put_name(out, node->name);
        break;
    case TB_SHADER_NODE_BOOL:
        put(out, node->integer != 0 ? "true" : "false");
        break;
    case TB_SHADER_NODE_NAME:
        print_name(out, node);
        break;
    case TB_SHADER_NODE_UNARY:
        print_unary(out, node);
        break;
    case TB_SHADER_NODE_BINARY:
    case TB_SHADER_NODE_ASSIGN:
        print_operation(out, tb_shader_operator_spelling(node->op), node->left, node->right);
        break;
    case TB_SHADER_NODE_CONDITIONAL:
        print_conditional(out, node);
        break;
    case TB_SHADER_NODE_CALL:
        print_call(out, node);
        break;
    case TB_SHADER_NODE_CONSTRUCT:
        print_construct(out, node);
        break;
    case TB_SHADER_NODE_CAST:
        print_cast(out, node);
        break;
    case TB_SHADER_NODE_INDEX:
        print_operation(out, "[]", node->left, node->right);
        break;
    case TB_SHADER_NODE_MEMBER:
        print_member(out, node);
        break;
    case TB_SHADER_NODE_INITIALIZER_LIST:
        put(out, "(list");
        put_children(out, node->children);
        put(out, ")");
        break;
    case TB_SHADER_NODE_SIZEOF:
        print_sizeof(out, node);
        break;
    }
}

static void print_node(printer *out, const tb_shader_node *node)
{
    if (node == NULL)
    {
        put(out, "<error>");
        return;
    }
    print_by_kind(out, node);
}

const char *tb_shader_syntax_print(const tb_shader_syntax *syntax, tb_arena *arena)
{
    tb_allocator allocator = tb_arena_allocator(arena);
    printer out = {0};
    out.text = TB_ARRAY_CREATE(&allocator, char);
    for (const tb_shader_node *node = syntax->declarations; node != NULL; node = node->next)
    {
        print_node(&out, node);
        put(&out, "\n");
    }
    const char nul = '\0';
    (void)tb_array_push(out.text, &nul);
    return tb_array_data(out.text);
}
