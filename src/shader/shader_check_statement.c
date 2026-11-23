/* Checking statements: their conditions, what `return` returns, where
 * `break`, `continue` and `case` may be, and whether a function that returns
 * a value always does. */
#include "shader_check_internal.h"

static void check_statement(tb_shader_checker *checker, tb_shader_node *node);

static void check_block(tb_shader_checker *checker, tb_shader_node *node)
{
    const size_t mark = tb_shader_check_push_scope(checker);
    for (tb_shader_node *statement = node->children; statement != NULL && !checker->stopped;
         statement = statement->next)
    {
        check_statement(checker, statement);
    }
    tb_shader_check_pop_scope(checker, mark);
}

/* A statement that opens a scope of its own when it is not a block, as the
 * body of `if` does: `if (a) int x = 1;` declares x for that body only. */
static void check_scoped(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node == NULL)
    {
        return;
    }
    const size_t mark = tb_shader_check_push_scope(checker);
    check_statement(checker, node);
    tb_shader_check_pop_scope(checker, mark);
}

static void check_declaration_statement(tb_shader_checker *checker, tb_shader_node *node)
{
    for (tb_shader_node *declared = node->children; declared != NULL; declared = declared->next)
    {
        if (declared->kind == TB_SHADER_NODE_VARIABLE)
        {
            tb_shader_check_variable(checker, declared);
        }
        else
        {
            tb_shader_check_local_declaration(checker, declared);
        }
    }
}

static void check_loop_body(tb_shader_checker *checker, tb_shader_node *body)
{
    checker->loop_depth++;
    checker->break_depth++;
    check_scoped(checker, body);
    checker->break_depth--;
    checker->loop_depth--;
}

static void check_for(tb_shader_checker *checker, tb_shader_node *node)
{
    const size_t mark = tb_shader_check_push_scope(checker);
    if (node->init != NULL)
    {
        check_statement(checker, node->init);
    }
    if (node->condition != NULL)
    {
        node->condition = tb_shader_check_condition(checker, node->condition, "a for loop");
    }
    if (node->step != NULL)
    {
        node->step = tb_shader_check_expression(checker, node->step);
    }
    check_loop_body(checker, node->body);
    tb_shader_check_pop_scope(checker, mark);
}

static bool is_switchable(const tb_shader_type *type)
{
    if (type->type_class == TB_SHADER_CLASS_ENUM || type->type_class == TB_SHADER_CLASS_ERROR)
    {
        return true;
    }
    return type->type_class == TB_SHADER_CLASS_SCALAR && tb_shader_scalar_is_integer(type->scalar);
}

static void check_switch(tb_shader_checker *checker, tb_shader_node *node)
{
    node->condition = tb_shader_check_expression(checker, node->condition);
    const tb_shader_type *const type = node->condition->checked;
    if (!is_switchable(type))
    {
        tb_shader_check_error(checker, node->condition->location,
                              "a switch needs an integer, not '%s'", type->name);
    }
    tb_shader_node *const outer = checker->innermost_switch;
    checker->innermost_switch = node;
    checker->break_depth++;
    check_scoped(checker, node->body);
    checker->break_depth--;
    checker->innermost_switch = outer;
}

/* Whether a case's value repeats one before it in the same switch. */
static const tb_shader_node *earlier_case(const tb_shader_node *block, const tb_shader_node *label)
{
    for (const tb_shader_node *other = block->children; other != NULL && other != label;
         other = other->next)
    {
        if (other->kind == TB_SHADER_NODE_CASE && other->value != NULL &&
            other->value->is_constant && other->value->constant.bits == label->value->constant.bits)
        {
            return other;
        }
        if (other->kind == TB_SHADER_NODE_DEFAULT && label->kind == TB_SHADER_NODE_DEFAULT)
        {
            return other;
        }
    }
    return NULL;
}

static void check_label(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_node *const owner = checker->innermost_switch;
    const char *const word = node->kind == TB_SHADER_NODE_CASE ? "case" : "default";
    if (owner == NULL)
    {
        tb_shader_check_error(checker, node->location, "'%s' is not in a switch", word);
        return;
    }
    if (node->kind == TB_SHADER_NODE_CASE)
    {
        int64_t value = 0;
        if (!tb_shader_check_constant_integer(checker, node->value, &value))
        {
            return;
        }
    }
    /* Labels are checked where they stand: directly in the switch's block. */
    const tb_shader_node *const block = owner->body;
    if (block == NULL || block->kind != TB_SHADER_NODE_BLOCK)
    {
        return;
    }
    const tb_shader_node *const earlier = earlier_case(block, node);
    if (earlier == NULL)
    {
        return;
    }
    if (node->kind == TB_SHADER_NODE_DEFAULT)
    {
        tb_shader_check_error(checker, node->location, "a switch has one 'default'");
        return;
    }
    tb_shader_check_error(checker, node->location, "duplicate case value %lld",
                          (long long)node->value->constant.bits);
}

static void check_return(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *const returned = checker->function->type;
    const bool returns_void = returned->type_class == TB_SHADER_CLASS_VOID;
    if (node->value == NULL)
    {
        if (!returns_void && returned->type_class != TB_SHADER_CLASS_ERROR)
        {
            tb_shader_check_error(checker, node->location, "'%.*s' must return a '%s'",
                                  (int)checker->function->name.length, checker->function->name.text,
                                  returned->name);
        }
        return;
    }
    node->value = tb_shader_check_expression(checker, node->value);
    if (returns_void)
    {
        tb_shader_check_error(checker, node->value->location, "'%.*s' returns no value",
                              (int)checker->function->name.length, checker->function->name.text);
        return;
    }
    node->value = tb_shader_check_convert(checker, node->value, returned);
}

static void check_jump(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node->kind == TB_SHADER_NODE_BREAK && checker->break_depth == 0)
    {
        tb_shader_check_error(checker, node->location, "'break' is not in a loop or a switch");
    }
    if (node->kind == TB_SHADER_NODE_CONTINUE && checker->loop_depth == 0)
    {
        tb_shader_check_error(checker, node->location, "'continue' is not in a loop");
    }
}

static void check_statement(tb_shader_checker *checker, tb_shader_node *node)
{
    switch (node->kind)
    {
    case TB_SHADER_NODE_BLOCK:
        check_block(checker, node);
        break;
    case TB_SHADER_NODE_DECLARATION:
        check_declaration_statement(checker, node);
        break;
    case TB_SHADER_NODE_EXPRESSION_STATEMENT:
        node->value = tb_shader_check_expression(checker, node->value);
        break;
    case TB_SHADER_NODE_IF:
        node->condition = tb_shader_check_condition(checker, node->condition, "an if statement");
        check_scoped(checker, node->body);
        check_scoped(checker, node->else_body);
        break;
    case TB_SHADER_NODE_WHILE:
        node->condition = tb_shader_check_condition(checker, node->condition, "a while loop");
        check_loop_body(checker, node->body);
        break;
    case TB_SHADER_NODE_DO:
        check_loop_body(checker, node->body);
        node->condition = tb_shader_check_condition(checker, node->condition, "a do loop");
        break;
    case TB_SHADER_NODE_FOR:
        check_for(checker, node);
        break;
    case TB_SHADER_NODE_SWITCH:
        check_switch(checker, node);
        break;
    case TB_SHADER_NODE_CASE:
    case TB_SHADER_NODE_DEFAULT:
        check_label(checker, node);
        break;
    case TB_SHADER_NODE_RETURN:
        check_return(checker, node);
        break;
    case TB_SHADER_NODE_BREAK:
    case TB_SHADER_NODE_CONTINUE:
        check_jump(checker, node);
        break;
    case TB_SHADER_NODE_DISCARD:
    case TB_SHADER_NODE_EMPTY:
    case TB_SHADER_NODE_PRAGMA:
        break;
    case TB_SHADER_NODE_STRUCT:
    case TB_SHADER_NODE_INTERFACE:
    case TB_SHADER_NODE_FUNCTION:
    case TB_SHADER_NODE_VARIABLE:
    case TB_SHADER_NODE_PARAMETER:
    case TB_SHADER_NODE_CBUFFER:
    case TB_SHADER_NODE_TBUFFER:
    case TB_SHADER_NODE_TYPEDEF:
    case TB_SHADER_NODE_NAMESPACE:
    case TB_SHADER_NODE_ENUM:
    case TB_SHADER_NODE_ENUMERATOR:
    case TB_SHADER_NODE_TEMPLATE_PARAMETER:
    case TB_SHADER_NODE_INTEGER:
    case TB_SHADER_NODE_FLOAT:
    case TB_SHADER_NODE_BOOL:
    case TB_SHADER_NODE_STRING:
    case TB_SHADER_NODE_NAME:
    case TB_SHADER_NODE_UNARY:
    case TB_SHADER_NODE_BINARY:
    case TB_SHADER_NODE_ASSIGN:
    case TB_SHADER_NODE_CONDITIONAL:
    case TB_SHADER_NODE_CALL:
    case TB_SHADER_NODE_CONSTRUCT:
    case TB_SHADER_NODE_CAST:
    case TB_SHADER_NODE_INDEX:
    case TB_SHADER_NODE_MEMBER:
    case TB_SHADER_NODE_INITIALIZER_LIST:
    case TB_SHADER_NODE_SIZEOF:
    case TB_SHADER_NODE_CONVERT:
        /* Not statements: the parser makes none of these one. */
        break;
    }
}

/* -------------------------------------------------------------------------
 * Whether control can reach the end of a function.
 * ------------------------------------------------------------------------- */

static bool always_returns(const tb_shader_node *node);

static bool contains_break(const tb_shader_node *node)
{
    if (node == NULL)
    {
        return false;
    }
    if (node->kind == TB_SHADER_NODE_BREAK)
    {
        return true;
    }
    /* A break in an inner loop or switch leaves that one. */
    const bool opens_breakable = node->kind == TB_SHADER_NODE_FOR || node->kind == TB_SHADER_NODE_WHILE ||
                                 node->kind == TB_SHADER_NODE_DO || node->kind == TB_SHADER_NODE_SWITCH;
    if (opens_breakable)
    {
        return false;
    }
    if (node->kind == TB_SHADER_NODE_IF)
    {
        return contains_break(node->body) || contains_break(node->else_body);
    }
    for (const tb_shader_node *child = node->children;
         child != NULL && node->kind == TB_SHADER_NODE_BLOCK; child = child->next)
    {
        if (contains_break(child))
        {
            return true;
        }
    }
    return false;
}

/* A loop whose condition is absent or a constant true never ends but by
 * `break` or `return`. */
static bool is_endless(const tb_shader_node *node)
{
    const tb_shader_node *const condition = node->condition;
    const bool forever = condition == NULL || (condition->is_constant && !condition->constant.is_float &&
                                               condition->constant.bits != 0);
    return forever && !contains_break(node->body);
}

static bool always_returns(const tb_shader_node *node)
{
    if (node == NULL)
    {
        return false;
    }
    const tb_shader_node_kind kind = node->kind;
    if (kind == TB_SHADER_NODE_RETURN || kind == TB_SHADER_NODE_DISCARD)
    {
        return true;
    }
    if (kind == TB_SHADER_NODE_BLOCK)
    {
        for (const tb_shader_node *child = node->children; child != NULL; child = child->next)
        {
            if (always_returns(child))
            {
                return true;
            }
        }
        return false;
    }
    if (kind == TB_SHADER_NODE_IF)
    {
        return always_returns(node->body) && always_returns(node->else_body);
    }
    if (kind == TB_SHADER_NODE_DO)
    {
        return always_returns(node->body) || is_endless(node);
    }
    if (kind == TB_SHADER_NODE_WHILE || kind == TB_SHADER_NODE_FOR)
    {
        return is_endless(node);
    }
    return false;
}

void tb_shader_check_body(tb_shader_checker *checker, tb_shader_node *function)
{
    tb_shader_symbol *const outer_function = checker->function;
    tb_shader_node *const outer_switch = checker->innermost_switch;
    const size_t outer_loops = checker->loop_depth;
    const size_t outer_breaks = checker->break_depth;
    checker->function = function->symbol;
    checker->innermost_switch = NULL;
    checker->loop_depth = 0;
    checker->break_depth = 0;
    const size_t mark = tb_shader_check_push_scope(checker);
    for (tb_shader_node *parameter = function->parameters; parameter != NULL;
         parameter = parameter->next)
    {
        if (parameter->symbol != NULL)
        {
            (void)tb_shader_check_declare(checker, parameter->symbol, parameter->location);
        }
    }
    check_block(checker, function->body);
    tb_shader_check_pop_scope(checker, mark);
    const tb_shader_type *const returned = function->checked;
    const bool returns_value = returned->type_class != TB_SHADER_CLASS_VOID &&
                               returned->type_class != TB_SHADER_CLASS_ERROR;
    if (returns_value && !always_returns(function->body))
    {
        tb_shader_check_error(checker, function->location,
                              "'%.*s' may reach its end without returning a value",
                              (int)function->name.length, function->name.text);
    }
    checker->function = outer_function;
    checker->innermost_switch = outer_switch;
    checker->loop_depth = outer_loops;
    checker->break_depth = outer_breaks;
}
