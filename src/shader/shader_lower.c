/* Lowering a checked shader into the compiler's own form: its types,
 * globals and functions, and the statements of each function. Loops become
 * `loop` with the condition tested at the top (or in the continuing block,
 * for `do`), a `switch` becomes cases that say whether they fall through,
 * and a parameter passed by value becomes a local the function may change. */
#include "shader_lower_internal.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"

/* -------------------------------------------------------------------------
 * Diagnostics.
 * ------------------------------------------------------------------------- */

void tb_shader_lower_unsupported(tb_shader_lowerer *lowerer, tb_shader_location location,
                                 const char *what)
{
    const char *const message =
        tb_fmt_string(&lowerer->allocator, "%s is not supported by the compiler yet", what);
    const tb_shader_diagnostic diagnostic = {TB_SHADER_ERROR, lowerer->files[location.file],
                                             location.line, location.column, message};
    (void)tb_array_push(lowerer->diagnostics, &diagnostic);
    lowerer->error_count++;
}

/* -------------------------------------------------------------------------
 * Maps, keyed by what checking made.
 * ------------------------------------------------------------------------- */

static uint64_t pointer_hash(const void *key)
{
    uint64_t value = (uint64_t)(uintptr_t)(*(const void *const *)key);
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return value;
}

static bool pointer_equal(const void *left, const void *right)
{
    return *(const void *const *)left == *(const void *const *)right;
}

static tb_table *new_map(tb_shader_lowerer *lowerer)
{
    return tb_table_create(&lowerer->allocator, 64, pointer_hash, pointer_equal);
}

void tb_shader_lower_bind(tb_shader_lowerer *lowerer, tb_table *map, const void *key,
                          uint32_t value)
{
    const void **const stored_key = TB_ARENA_NEW(lowerer->arena, const void *);
    uint32_t *const stored_value = TB_ARENA_NEW(lowerer->arena, uint32_t);
    *stored_key = key;
    *stored_value = value;
    (void)tb_table_put(map, stored_key, stored_value);
}

uint32_t tb_shader_lower_bound(const tb_shader_lowerer *lowerer, const tb_table *map,
                               const void *key)
{
    (void)lowerer;
    const uint32_t *const found = tb_table_get(map, &key);
    return found == NULL ? TB_SHADER_FORM_NONE : *found;
}

#define BINDING_KIND_SHIFT 28
#define BINDING_INDEX_MASK ((UINT32_C(1) << BINDING_KIND_SHIFT) - 1)

uint32_t tb_shader_lower_binding(tb_shader_binding_kind kind, uint32_t index)
{
    TB_ASSERT(index <= BINDING_INDEX_MASK);
    return ((uint32_t)kind << BINDING_KIND_SHIFT) | (index & BINDING_INDEX_MASK);
}

tb_shader_binding_kind tb_shader_lower_binding_kind(uint32_t binding)
{
    return (tb_shader_binding_kind)(binding >> BINDING_KIND_SHIFT);
}

uint32_t tb_shader_lower_binding_index(uint32_t binding)
{
    return binding & BINDING_INDEX_MASK;
}

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

static const char *name_text(tb_shader_lowerer *lowerer, tb_shader_name name)
{
    return tb_fmt_string(&lowerer->allocator, "%.*s", (int)name.length, name.text);
}

static uint32_t struct_type(tb_shader_lowerer *lowerer, const tb_shader_type *type,
                            tb_shader_location location)
{
    const tb_shader_structure *const structure = type->structure;
    tb_shader_form_member *const members =
        TB_ARENA_NEW_ARRAY(lowerer->arena, tb_shader_form_member, structure->field_count + 1);
    for (size_t index = 0; index < structure->field_count; index++)
    {
        const tb_shader_symbol *const field = structure->fields[index];
        members[index].name = name_text(lowerer, field->name);
        members[index].type = tb_shader_lower_type(lowerer, field->type, location);
        if (members[index].type == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    return tb_shader_form_struct(lowerer->form, type->name, members,
                                 (uint32_t)structure->field_count);
}

static uint32_t new_type(tb_shader_lowerer *lowerer, const tb_shader_type *type,
                         tb_shader_location location)
{
    tb_shader_form *const form = lowerer->form;
    const tb_shader_scalar scalar = tb_shader_concrete_scalar(type->scalar);
    switch (type->type_class)
    {
    case TB_SHADER_CLASS_VOID:
        return tb_shader_form_void(form);
    case TB_SHADER_CLASS_SCALAR:
    case TB_SHADER_CLASS_ENUM:
        return tb_shader_form_scalar(form, scalar);
    case TB_SHADER_CLASS_VECTOR:
        return tb_shader_form_vector(form, scalar, type->columns);
    case TB_SHADER_CLASS_MATRIX:
        return tb_shader_form_matrix(form, scalar, type->rows, type->columns);
    case TB_SHADER_CLASS_ARRAY: {
        if (type->length == 0)
        {
            tb_shader_lower_unsupported(lowerer, location, "an array without a size");
            return TB_SHADER_FORM_NONE;
        }
        const uint32_t element = tb_shader_lower_type(lowerer, type->element, location);
        return element == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE
                                              : tb_shader_form_array(form, element, type->length);
    }
    case TB_SHADER_CLASS_STRUCT:
        return struct_type(lowerer, type, location);
    case TB_SHADER_CLASS_OBJECT:
        tb_shader_lower_unsupported(lowerer, location, "a resource or another object");
        return TB_SHADER_FORM_NONE;
    case TB_SHADER_CLASS_STRING:
        return tb_shader_form_string(form);
    case TB_SHADER_CLASS_ERROR:
        break;
    }
    tb_shader_lower_unsupported(lowerer, location, "a type that did not check");
    return TB_SHADER_FORM_NONE;
}

uint32_t tb_shader_lower_type(tb_shader_lowerer *lowerer, const tb_shader_type *type,
                              tb_shader_location location)
{
    const uint32_t known = tb_shader_lower_bound(lowerer, lowerer->types, type);
    if (known != TB_SHADER_FORM_NONE)
    {
        return known;
    }
    const uint32_t made = new_type(lowerer, type, location);
    if (made != TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_bind(lowerer, lowerer->types, type, made);
    }
    return made;
}

/* -------------------------------------------------------------------------
 * Blocks and statements.
 * ------------------------------------------------------------------------- */

static tb_shader_form_statement new_statement(tb_shader_form_statement_kind kind)
{
    tb_shader_form_statement statement = {0};
    statement.kind = kind;
    statement.next = TB_SHADER_FORM_NONE;
    statement.operands[0] = TB_SHADER_FORM_NONE;
    statement.operands[1] = TB_SHADER_FORM_NONE;
    statement.first = TB_SHADER_FORM_NONE;
    statement.accept = TB_SHADER_FORM_NONE;
    statement.reject = TB_SHADER_FORM_NONE;
    statement.body = TB_SHADER_FORM_NONE;
    statement.continuing = TB_SHADER_FORM_NONE;
    statement.function = TB_SHADER_FORM_NONE;
    statement.result = TB_SHADER_FORM_NONE;
    return statement;
}

/* Add a statement to the end of the current block. */
static uint32_t append(tb_shader_lowerer *lowerer, const tb_shader_form_statement *statement)
{
    tb_shader_form_function *const function = lowerer->function;
    const uint32_t index = tb_shader_form_add_statement(function, statement);
    if (lowerer->last == TB_SHADER_FORM_NONE)
    {
        tb_shader_form_statement_at(function, lowerer->block)->first = index;
    }
    else
    {
        tb_shader_form_statement_at(function, lowerer->last)->next = index;
    }
    lowerer->last = index;
    return index;
}

void tb_shader_lower_flush(tb_shader_lowerer *lowerer)
{
    const uint32_t end = (uint32_t)tb_shader_form_expression_count(lowerer->function);
    bool has_work = false;
    for (uint32_t index = lowerer->emit_start; index < end && !has_work; index++)
    {
        has_work =
            tb_shader_form_is_emitted(tb_shader_form_expression_at(lowerer->function, index)->kind);
    }
    if (has_work)
    {
        tb_shader_form_statement emit = new_statement(TB_SHADER_FORM_EMIT);
        emit.operands[0] = lowerer->emit_start;
        emit.operands[1] = end;
        (void)append(lowerer, &emit);
    }
    lowerer->emit_start = end;
}

uint32_t tb_shader_lower_statement(tb_shader_lowerer *lowerer,
                                   const tb_shader_form_statement *statement)
{
    tb_shader_lower_flush(lowerer);
    const uint32_t index = append(lowerer, statement);
    const tb_shader_form_statement_kind kind = statement->kind;
    if (kind == TB_SHADER_FORM_BREAK || kind == TB_SHADER_FORM_CONTINUE ||
        kind == TB_SHADER_FORM_RETURN)
    {
        lowerer->ended = true;
    }
    return index;
}

uint32_t tb_shader_lower_open_block(tb_shader_lowerer *lowerer, tb_shader_block_mark *mark)
{
    tb_shader_lower_flush(lowerer);
    mark->block = lowerer->block;
    mark->last = lowerer->last;
    mark->ended = lowerer->ended;
    const tb_shader_form_statement block = new_statement(TB_SHADER_FORM_BLOCK);
    lowerer->block = tb_shader_form_add_statement(lowerer->function, &block);
    lowerer->last = TB_SHADER_FORM_NONE;
    lowerer->ended = false;
    return lowerer->block;
}

void tb_shader_lower_close_block(tb_shader_lowerer *lowerer, const tb_shader_block_mark *mark)
{
    tb_shader_lower_flush(lowerer);
    lowerer->block = mark->block;
    lowerer->last = mark->last;
    lowerer->ended = mark->ended;
}

static void lower_statement(tb_shader_lowerer *lowerer, tb_shader_node *node);

/* The statements of a list, up to the first that ends the block. */
static void lower_statements(tb_shader_lowerer *lowerer, tb_shader_node *first)
{
    for (tb_shader_node *node = first; node != NULL && !lowerer->ended; node = node->next)
    {
        lower_statement(lowerer, node);
    }
}

/* A block of its own holding one statement of the source. */
static uint32_t lower_into_block(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    tb_shader_block_mark mark = {0};
    const uint32_t block = tb_shader_lower_open_block(lowerer, &mark);
    if (node != NULL)
    {
        lower_statement(lowerer, node);
    }
    tb_shader_lower_close_block(lowerer, &mark);
    return block;
}

static void add_jump(tb_shader_lowerer *lowerer, tb_shader_form_statement_kind kind)
{
    const tb_shader_form_statement jump = new_statement(kind);
    (void)tb_shader_lower_statement(lowerer, &jump);
}

/* `if (!condition) break;` at the top of a loop's body. */
static void break_unless(tb_shader_lowerer *lowerer, tb_shader_node *condition)
{
    const uint32_t value = tb_shader_lower_value(lowerer, condition);
    if (value == TB_SHADER_FORM_NONE)
    {
        return;
    }
    tb_shader_form_statement test = new_statement(TB_SHADER_FORM_IF);
    test.operands[0] = tb_shader_lower_not(lowerer, value);
    tb_shader_block_mark mark = {0};
    tb_shader_lower_flush(lowerer);
    test.accept = tb_shader_lower_open_block(lowerer, &mark);
    add_jump(lowerer, TB_SHADER_FORM_BREAK);
    tb_shader_lower_close_block(lowerer, &mark);
    (void)tb_shader_lower_statement(lowerer, &test);
}

/* `for`, `while` and `do`: a loop whose condition is tested first, or, for
 * `do`, in the continuing block, where `continue` goes too. */
static void lower_loop(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node->kind == TB_SHADER_NODE_FOR && node->init != NULL)
    {
        lower_statement(lowerer, node->init);
    }
    tb_shader_form_statement loop = new_statement(TB_SHADER_FORM_LOOP);
    tb_shader_block_mark mark = {0};
    loop.body = tb_shader_lower_open_block(lowerer, &mark);
    if (node->kind != TB_SHADER_NODE_DO && node->condition != NULL)
    {
        break_unless(lowerer, node->condition);
    }
    lower_statement(lowerer, node->body);
    tb_shader_lower_close_block(lowerer, &mark);
    loop.continuing = tb_shader_lower_open_block(lowerer, &mark);
    if (node->kind == TB_SHADER_NODE_FOR && node->step != NULL)
    {
        tb_shader_lower_effect(lowerer, node->step);
    }
    if (node->kind == TB_SHADER_NODE_DO)
    {
        const uint32_t value = tb_shader_lower_value(lowerer, node->condition);
        if (value != TB_SHADER_FORM_NONE)
        {
            loop.operands[0] = tb_shader_lower_not(lowerer, value);
        }
    }
    tb_shader_lower_close_block(lowerer, &mark);
    (void)tb_shader_lower_statement(lowerer, &loop);
}

static void lower_if(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t condition = tb_shader_lower_value(lowerer, node->condition);
    if (condition == TB_SHADER_FORM_NONE)
    {
        return;
    }
    tb_shader_lower_flush(lowerer);
    tb_shader_form_statement test = new_statement(TB_SHADER_FORM_IF);
    test.operands[0] = condition;
    test.accept = lower_into_block(lowerer, node->body);
    if (node->else_body != NULL)
    {
        test.reject = lower_into_block(lowerer, node->else_body);
    }
    (void)tb_shader_lower_statement(lowerer, &test);
}

/* -------------------------------------------------------------------------
 * Switch: each label starts a case; a case whose statements end without
 * `break` falls through to the next, as in C.
 * ------------------------------------------------------------------------- */

typedef struct switch_lowering
{
    uint32_t first;
    uint32_t last; /* The case being lowered, or NONE before the first label. */
    tb_shader_block_mark mark;
} switch_lowering;

static void finish_case(tb_shader_lowerer *lowerer, switch_lowering *cases, bool ends_in_break)
{
    if (cases->last == TB_SHADER_FORM_NONE)
    {
        return;
    }
    const bool ended = lowerer->ended;
    tb_shader_lower_close_block(lowerer, &cases->mark);
    tb_shader_form_statement_at(lowerer->function, cases->last)->falls_through =
        !ends_in_break && !ended;
}

static void start_case(tb_shader_lowerer *lowerer, switch_lowering *cases, tb_shader_node *label)
{
    tb_shader_form_statement entry = new_statement(TB_SHADER_FORM_CASE);
    entry.is_default = label->kind == TB_SHADER_NODE_DEFAULT;
    if (!entry.is_default)
    {
        entry.value = label->value->is_constant ? label->value->constant.bits : 0;
    }
    const uint32_t index = tb_shader_form_add_statement(lowerer->function, &entry);
    if (cases->last == TB_SHADER_FORM_NONE)
    {
        cases->first = index;
    }
    else
    {
        tb_shader_form_statement_at(lowerer->function, cases->last)->next = index;
    }
    cases->last = index;
    const uint32_t body = tb_shader_lower_open_block(lowerer, &cases->mark);
    tb_shader_form_statement_at(lowerer->function, index)->body = body;
}

static void lower_switch(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t selector = tb_shader_lower_value(lowerer, node->condition);
    if (selector == TB_SHADER_FORM_NONE)
    {
        return;
    }
    tb_shader_lower_flush(lowerer);
    switch_lowering cases = {TB_SHADER_FORM_NONE, TB_SHADER_FORM_NONE, {0}};
    bool skipping = true; /* Before the first label, or after a case's own `break`. */
    tb_shader_node *const first = node->body != NULL && node->body->kind == TB_SHADER_NODE_BLOCK
                                      ? node->body->children
                                      : NULL;
    for (tb_shader_node *child = first; child != NULL; child = child->next)
    {
        if (child->kind == TB_SHADER_NODE_CASE || child->kind == TB_SHADER_NODE_DEFAULT)
        {
            if (!skipping)
            {
                finish_case(lowerer, &cases, false);
            }
            start_case(lowerer, &cases, child);
            skipping = false;
            continue;
        }
        if (skipping || lowerer->ended)
        {
            continue;
        }
        if (child->kind == TB_SHADER_NODE_BREAK)
        {
            finish_case(lowerer, &cases, true);
            skipping = true;
            continue;
        }
        lower_statement(lowerer, child);
    }
    if (!skipping)
    {
        finish_case(lowerer, &cases, false);
    }
    if (cases.last != TB_SHADER_FORM_NONE)
    {
        tb_shader_form_statement_at(lowerer->function, cases.last)->falls_through = false;
    }
    tb_shader_form_statement statement = new_statement(TB_SHADER_FORM_SWITCH);
    statement.operands[0] = selector;
    statement.first = cases.first;
    (void)tb_shader_lower_statement(lowerer, &statement);
}

/* -------------------------------------------------------------------------
 * Declarations in a function.
 * ------------------------------------------------------------------------- */

/* A static variable: a private global, whose first value, if it has one,
 * the form's initializer stores. */
static void declare_static(tb_shader_lowerer *lowerer, tb_shader_node *node, const char *name)
{
    const tb_shader_symbol *const symbol = node->symbol;
    tb_shader_form_global global = {0};
    global.name = name;
    global.type = tb_shader_lower_type(lowerer, symbol->type, node->location);
    global.space = TB_SHADER_FORM_PRIVATE;
    if (global.type == TB_SHADER_FORM_NONE)
    {
        return;
    }
    const uint32_t index = tb_shader_form_add_global(lowerer->form, &global);
    tb_shader_lower_bind(lowerer, lowerer->bindings, symbol,
                         tb_shader_lower_binding(TB_SHADER_BINDING_GLOBAL, index));
    if (node->value != NULL)
    {
        const tb_shader_static_value first = {index, node->value, symbol->type};
        (void)tb_array_push(lowerer->statics, &first);
    }
}

static void declare_local(tb_shader_lowerer *lowerer, tb_shader_node *variable)
{
    const tb_shader_symbol *const symbol = variable->symbol;
    if (symbol == NULL)
    {
        return;
    }
    if (symbol->storage == TB_SHADER_STORAGE_STATIC && !symbol->has_value)
    {
        declare_static(lowerer, variable,
                       tb_fmt_string(&lowerer->allocator, "%s::%.*s", lowerer->function->name,
                                     (int)variable->name.length, variable->name.text));
        return;
    }
    const uint32_t type = tb_shader_lower_type(lowerer, symbol->type, variable->location);
    if (type == TB_SHADER_FORM_NONE)
    {
        return;
    }
    const uint32_t local =
        tb_shader_form_add_local(lowerer->function, name_text(lowerer, variable->name), type);
    tb_shader_lower_bind(lowerer, lowerer->bindings, symbol,
                         tb_shader_lower_binding(TB_SHADER_BINDING_LOCAL, local));
    if (variable->value == NULL)
    {
        return;
    }
    const uint32_t value = tb_shader_lower_initializer(lowerer, variable->value, symbol->type);
    if (value != TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, local), value);
    }
}

static void lower_declaration(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    for (tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        if (child->kind == TB_SHADER_NODE_VARIABLE)
        {
            declare_local(lowerer, child);
        }
    }
}

static void lower_return(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    tb_shader_form_statement statement = new_statement(TB_SHADER_FORM_RETURN);
    if (node->value != NULL)
    {
        statement.operands[0] = tb_shader_lower_value(lowerer, node->value);
        if (statement.operands[0] == TB_SHADER_FORM_NONE)
        {
            return;
        }
    }
    (void)tb_shader_lower_statement(lowerer, &statement);
}

static void lower_statement(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    switch (node->kind)
    {
    case TB_SHADER_NODE_BLOCK:
        lower_statements(lowerer, node->children);
        return;
    case TB_SHADER_NODE_DECLARATION:
        lower_declaration(lowerer, node);
        return;
    case TB_SHADER_NODE_EXPRESSION_STATEMENT:
        tb_shader_lower_effect(lowerer, node->value);
        return;
    case TB_SHADER_NODE_IF:
        lower_if(lowerer, node);
        return;
    case TB_SHADER_NODE_FOR:
    case TB_SHADER_NODE_WHILE:
    case TB_SHADER_NODE_DO:
        lower_loop(lowerer, node);
        return;
    case TB_SHADER_NODE_SWITCH:
        lower_switch(lowerer, node);
        return;
    case TB_SHADER_NODE_BREAK:
        add_jump(lowerer, TB_SHADER_FORM_BREAK);
        return;
    case TB_SHADER_NODE_CONTINUE:
        add_jump(lowerer, TB_SHADER_FORM_CONTINUE);
        return;
    case TB_SHADER_NODE_RETURN:
        lower_return(lowerer, node);
        return;
    case TB_SHADER_NODE_DISCARD:
        add_jump(lowerer, TB_SHADER_FORM_DISCARD);
        return;
    case TB_SHADER_NODE_EMPTY:
    case TB_SHADER_NODE_CASE:
    case TB_SHADER_NODE_DEFAULT:
        return;
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
    case TB_SHADER_NODE_PRAGMA:
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
        break;
    }
    tb_shader_lower_unsupported(lowerer, node->location, "this statement");
}

/* -------------------------------------------------------------------------
 * Functions and globals.
 * ------------------------------------------------------------------------- */

static tb_shader_form_direction direction_of(const tb_shader_node *parameter)
{
    if ((parameter->modifiers & TB_SHADER_MODIFIER_INOUT) != 0 ||
        ((parameter->modifiers & TB_SHADER_MODIFIER_IN) != 0 &&
         (parameter->modifiers & TB_SHADER_MODIFIER_OUT) != 0))
    {
        return TB_SHADER_FORM_INOUT;
    }
    return (parameter->modifiers & TB_SHADER_MODIFIER_OUT) != 0 ? TB_SHADER_FORM_OUT
                                                                : TB_SHADER_FORM_IN;
}

bool tb_shader_lower_takes_this(const tb_shader_symbol *function)
{
    return function->owner != NULL && function->declaration != NULL &&
           (function->declaration->modifiers & TB_SHADER_MODIFIER_STATIC) == 0;
}

/* A function's name in the form: a method's is qualified by its struct's. */
static const char *function_name(tb_shader_lowerer *lowerer, const tb_shader_symbol *symbol)
{
    const tb_shader_name name = symbol->name;
    size_t start = name.length;
    while (start >= 2 && !(name.text[start - 2] == ':' && name.text[start - 1] == ':'))
    {
        start--;
    }
    if (symbol->owner == NULL || start >= 2)
    {
        return name_text(lowerer, name);
    }
    return tb_fmt_string(&lowerer->allocator, "%s::%.*s", symbol->owner->name, (int)name.length,
                         name.text);
}

/* A function of the form for a function with a body, its parameters typed;
 * a method's first is `this`, a pointer to its object. */
static void declare_function(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t result = tb_shader_lower_type(lowerer, node->checked, node->location);
    if (result == TB_SHADER_FORM_NONE || node->symbol == NULL)
    {
        return;
    }
    const uint32_t index =
        tb_shader_form_add_function(lowerer->form, function_name(lowerer, node->symbol), result);
    tb_shader_form_function *const function = tb_shader_form_function_at(lowerer->form, index);
    if (tb_shader_lower_takes_this(node->symbol))
    {
        tb_shader_form_parameter self = {0};
        self.name = "this";
        self.type = tb_shader_lower_type(lowerer, node->symbol->owner, node->location);
        self.direction = TB_SHADER_FORM_INOUT;
        if (self.type == TB_SHADER_FORM_NONE)
        {
            return;
        }
        (void)tb_shader_form_add_parameter(function, &self);
    }
    for (tb_shader_node *parameter = node->parameters; parameter != NULL;
         parameter = parameter->next)
    {
        tb_shader_form_parameter entry = {0};
        entry.name = name_text(lowerer, parameter->name);
        entry.type = tb_shader_lower_type(lowerer, parameter->checked, parameter->location);
        entry.direction = direction_of(parameter);
        if (entry.type == TB_SHADER_FORM_NONE)
        {
            return;
        }
        (void)tb_shader_form_add_parameter(function, &entry);
    }
    tb_shader_lower_bind(lowerer, lowerer->functions, node->symbol, index);
}

/* A parameter passed by value becomes a local holding it, which the body
 * may change; one passed `out` or `inout` is the pointer it was given. */
static void bind_parameters(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    uint32_t index = lowerer->has_this ? 1 : 0;
    for (tb_shader_node *parameter = node->parameters; parameter != NULL;
         parameter = parameter->next, index++)
    {
        const tb_shader_form_parameter *const entry =
            TB_ARRAY_AT(lowerer->function->parameters, tb_shader_form_parameter, index);
        if (entry->direction != TB_SHADER_FORM_IN)
        {
            tb_shader_lower_bind(lowerer, lowerer->bindings, parameter->symbol,
                                 tb_shader_lower_binding(TB_SHADER_BINDING_ARGUMENT, index));
            continue;
        }
        const uint32_t local =
            tb_shader_form_add_local(lowerer->function, entry->name, entry->type);
        tb_shader_lower_bind(lowerer, lowerer->bindings, parameter->symbol,
                             tb_shader_lower_binding(TB_SHADER_BINDING_LOCAL, local));
        tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, local),
                              tb_shader_lower_argument(lowerer, index));
    }
}

static void lower_function(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t index = tb_shader_lower_bound(lowerer, lowerer->functions, node->symbol);
    if (index == TB_SHADER_FORM_NONE)
    {
        return;
    }
    tb_shader_form_function *const function = tb_shader_form_function_at(lowerer->form, index);
    lowerer->function = function;
    lowerer->emit_start = 0;
    lowerer->block = function->body;
    lowerer->last = TB_SHADER_FORM_NONE;
    lowerer->ended = false;
    lowerer->has_this = tb_shader_lower_takes_this(node->symbol);
    bind_parameters(lowerer, node);
    lower_statements(lowerer, node->body->children);
    tb_shader_lower_flush(lowerer);
    lowerer->function = NULL;
    lowerer->has_this = false;
}

/* The form's initializer: a function that stores each static variable's
 * first value, in the order of the source. */
static void lower_initializer_function(tb_shader_lowerer *lowerer)
{
    const size_t count = tb_array_length(lowerer->statics);
    if (count == 0)
    {
        return;
    }
    const uint32_t index = tb_shader_form_add_function(lowerer->form, "static initializer",
                                                       tb_shader_form_void(lowerer->form));
    tb_shader_form_function *const function = tb_shader_form_function_at(lowerer->form, index);
    lowerer->form->initializer = index;
    lowerer->function = function;
    lowerer->emit_start = 0;
    lowerer->block = function->body;
    lowerer->last = TB_SHADER_FORM_NONE;
    lowerer->ended = false;
    for (size_t at = 0; at < count; at++)
    {
        const tb_shader_static_value *const first =
            TB_ARRAY_AT(lowerer->statics, tb_shader_static_value, at);
        const uint32_t value = tb_shader_lower_initializer(lowerer, first->value, first->type);
        if (value == TB_SHADER_FORM_NONE)
        {
            continue;
        }
        tb_shader_form_expression pointer = {0};
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(lowerer->form->globals, tb_shader_form_global, first->global);
        pointer.kind = TB_SHADER_FORM_GLOBAL;
        pointer.type = global->type;
        pointer.space = global->space;
        pointer.index = first->global;
        pointer.operands[0] = TB_SHADER_FORM_NONE;
        pointer.operands[1] = TB_SHADER_FORM_NONE;
        pointer.operands[2] = TB_SHADER_FORM_NONE;
        tb_shader_lower_store(lowerer, tb_shader_lower_add(lowerer, &pointer), value);
    }
    tb_shader_lower_flush(lowerer);
    lowerer->function = NULL;
}

static void declare_global(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const tb_shader_symbol *const symbol = node->symbol;
    if (symbol == NULL || symbol->has_value)
    {
        return;
    }
    const bool is_static = symbol->storage == TB_SHADER_STORAGE_STATIC;
    if (!is_static && symbol->storage != TB_SHADER_STORAGE_GROUPSHARED)
    {
        return;
    }
    if (is_static)
    {
        declare_static(lowerer, node, name_text(lowerer, symbol->name));
        return;
    }
    tb_shader_form_global global = {0};
    global.name = name_text(lowerer, symbol->name);
    global.type = tb_shader_lower_type(lowerer, symbol->type, node->location);
    global.space = is_static ? TB_SHADER_FORM_PRIVATE : TB_SHADER_FORM_GROUPSHARED;
    if (global.type == TB_SHADER_FORM_NONE)
    {
        return;
    }
    const uint32_t index = tb_shader_form_add_global(lowerer->form, &global);
    tb_shader_lower_bind(lowerer, lowerer->bindings, symbol,
                         tb_shader_lower_binding(TB_SHADER_BINDING_GLOBAL, index));
}

/* Declare what the form holds of a list of declarations, namespaces' too;
 * then, on the second pass, lower the bodies of the functions. */
static void lower_declarations(tb_shader_lowerer *lowerer, tb_shader_node *first, bool bodies)
{
    for (tb_shader_node *node = first; node != NULL; node = node->next)
    {
        if (node->kind == TB_SHADER_NODE_NAMESPACE || node->kind == TB_SHADER_NODE_STRUCT)
        {
            lower_declarations(lowerer, node->children, bodies);
        }
        else if (node->kind == TB_SHADER_NODE_FUNCTION && node->body != NULL)
        {
            if (bodies)
            {
                lower_function(lowerer, node);
            }
            else
            {
                declare_function(lowerer, node);
            }
        }
        else if (node->kind == TB_SHADER_NODE_VARIABLE && !bodies)
        {
            declare_global(lowerer, node);
        }
    }
}

tb_shader_result tb_shader_lower(const tb_shader_syntax *syntax, const tb_shader_checked *checked,
                                 tb_arena *arena, tb_shader_form *form, tb_shader_lowered *lowered)
{
    const tb_shader_lowered empty = {0};
    *lowered = empty;
    tb_shader_form_init(form, arena);
    if (checked->error_count > 0)
    {
        return TB_SHADER_FAILED;
    }
    tb_shader_lowerer lowerer = {0};
    lowerer.arena = arena;
    lowerer.allocator = tb_arena_allocator(arena);
    lowerer.form = form;
    lowerer.files = syntax->files;
    lowerer.diagnostics = TB_ARRAY_CREATE(&lowerer.allocator, tb_shader_diagnostic);
    lowerer.types = new_map(&lowerer);
    lowerer.functions = new_map(&lowerer);
    lowerer.bindings = new_map(&lowerer);
    lowerer.statics = TB_ARRAY_CREATE(&lowerer.allocator, tb_shader_static_value);
    lower_declarations(&lowerer, syntax->declarations, false);
    lower_declarations(&lowerer, syntax->declarations, true);
    lower_initializer_function(&lowerer);
    lowered->diagnostics = tb_array_data(lowerer.diagnostics);
    lowered->diagnostic_count = tb_array_length(lowerer.diagnostics);
    lowered->error_count = lowerer.error_count;
    return lowerer.error_count == 0 ? TB_SHADER_OK : TB_SHADER_FAILED;
}
