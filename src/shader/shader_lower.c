/* Lowering a checked shader into the compiler's own form: its types,
 * globals and functions, and the statements of each function. Loops become
 * `loop` with the condition tested at the top (or in the continuing block,
 * for `do`), a `switch` becomes cases that say whether they fall through,
 * and a parameter passed by value becomes a local the function may change. */
#include "shader_lower_internal.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"

#include "badger/decimal.h"
#include "badger/str.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Diagnostics.
 * ------------------------------------------------------------------------- */

void tb_shader_lower_error(tb_shader_lowerer *lowerer, tb_shader_location location,
                           const char *message)
{
    const tb_shader_diagnostic diagnostic = {TB_SHADER_ERROR, lowerer->files[location.file],
                                             location.line, location.column, message};
    (void)tb_array_push(lowerer->diagnostics, &diagnostic);
    lowerer->error_count++;
}

void tb_shader_lower_unsupported(tb_shader_lowerer *lowerer, tb_shader_location location,
                                 const char *what)
{
    tb_shader_lower_error(
        lowerer, location,
        tb_fmt_string(&lowerer->allocator, "%s is not supported by the compiler yet", what));
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

uint32_t tb_shader_lower_binding(tb_shader_variable_kind kind, uint32_t index)
{
    TB_ASSERT(index <= BINDING_INDEX_MASK);
    return ((uint32_t)kind << BINDING_KIND_SHIFT) | (index & BINDING_INDEX_MASK);
}

tb_shader_variable_kind tb_shader_lower_binding_kind(uint32_t binding)
{
    return (tb_shader_variable_kind)(binding >> BINDING_KIND_SHIFT);
}

uint32_t tb_shader_lower_binding_index(uint32_t binding)
{
    return binding & BINDING_INDEX_MASK;
}

#define MEMBER_SHIFT 16
#define MEMBER_MASK ((UINT32_C(1) << MEMBER_SHIFT) - 1)

uint32_t tb_shader_lower_member_binding(uint32_t global, uint32_t member)
{
    TB_ASSERT(global <= MEMBER_MASK && member <= MEMBER_MASK);
    return (global << MEMBER_SHIFT) | member;
}

uint32_t tb_shader_lower_member_global(uint32_t binding)
{
    return binding >> MEMBER_SHIFT;
}

uint32_t tb_shader_lower_member_index(uint32_t binding)
{
    return binding & MEMBER_MASK;
}

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

static const char *name_text(tb_shader_lowerer *lowerer, tb_shader_name name)
{
    return tb_fmt_string(&lowerer->allocator, "%.*s", (int)name.length, name.text);
}

/* `: SV_Position`: the annotation of a declaration that is a semantic, the
 * one written without arguments; NULL when it has none. */
static const char *semantic_of(tb_shader_lowerer *lowerer, const tb_shader_annotation *annotation)
{
    for (; annotation != NULL; annotation = annotation->next)
    {
        if (!annotation->has_arguments)
        {
            return name_text(lowerer, annotation->name);
        }
    }
    return NULL;
}

/* How a declaration lays out a matrix: as it says, or else as the
 * `#pragma pack_matrix` before it says; zero for neither. */
static uint64_t layout_of(const tb_shader_node *declaration)
{
    const uint64_t layout = declaration->modifiers & TB_SHADER_FORM_LAYOUT_MODIFIERS;
    if (layout != 0)
    {
        return layout;
    }
    return declaration->pack_matrix;
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
        members[index].packoffset = TB_SHADER_FORM_NONE;
        if (field->declaration != NULL)
        {
            members[index].semantic = semantic_of(lowerer, field->declaration->annotations);
            members[index].modifiers =
                (field->declaration->modifiers & TB_SHADER_FORM_INTERFACE_MODIFIERS) |
                layout_of(field->declaration);
        }
        if (members[index].type == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    return tb_shader_form_struct(lowerer->form, type->name, members,
                                 (uint32_t)structure->field_count);
}

/* What an object is in the form: a constant buffer is the struct it holds,
 * and a resource a handle of what it holds. */
static uint32_t object_type(tb_shader_lowerer *lowerer, const tb_shader_type *type,
                            tb_shader_location location)
{
    const tb_shader_object object = type->object;
    if (object == TB_SHADER_OBJECT_CONSTANT_BUFFER || object == TB_SHADER_OBJECT_TEXTURE_BUFFER)
    {
        return tb_shader_lower_type(lowerer, type->element, location);
    }

    /* What a shader holds as a handle: a resource, a sampler, an
     * acceleration structure, a RayQuery, a geometry shader's stream, a
     * tessellation shader's patch. The rest are written in no variable a
     * shader runs with. */
    const bool is_handle = tb_shader_object_info_of(object) != NULL &&
                           object != TB_SHADER_OBJECT_HIT_OBJECT &&
                           (object < TB_SHADER_OBJECT_STATE_OBJECT_CONFIG ||
                            object > TB_SHADER_OBJECT_PROCEDURAL_PRIMITIVE_HIT_GROUP);
    if (!is_handle)
    {
        tb_shader_lower_unsupported(lowerer, location, "this object");
        return TB_SHADER_FORM_NONE;
    }
    uint32_t element = TB_SHADER_FORM_NONE;
    if (type->element != NULL)
    {
        element = tb_shader_lower_type(lowerer, type->element, location);
        if (element == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    return tb_shader_form_resource(lowerer->form, type->object, element, type->length,
                                   type->second_length, type->name);
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
        const bool of_resources = type->element->type_class == TB_SHADER_CLASS_OBJECT;
        if (type->length == 0 && !of_resources)
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
        return object_type(lowerer, type, location);
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
                         tb_shader_lower_binding(TB_SHADER_VARIABLE_GLOBAL, index));
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
                         tb_shader_lower_binding(TB_SHADER_VARIABLE_LOCAL, local));
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

/* An argument of an attribute, which must be a literal or a name. */
static bool attribute_argument(tb_shader_lowerer *lowerer, const tb_shader_node *node,
                               tb_shader_form_argument *out)
{
    if (node->kind == TB_SHADER_NODE_INTEGER)
    {
        out->kind = TB_SHADER_FORM_ARGUMENT_INTEGER;
        out->integer = node->integer;
        return true;
    }
    if (node->kind == TB_SHADER_NODE_FLOAT)
    {
        out->kind = TB_SHADER_FORM_ARGUMENT_FLOAT;
        const size_t length = node->name.length - node->suffix.length;
        return tb_decimal_parse(node->name.text, length, &out->value) == TB_DECIMAL_OK;
    }
    if (node->kind == TB_SHADER_NODE_STRING)
    {
        out->kind = TB_SHADER_FORM_ARGUMENT_STRING;
        out->text = tb_fmt_string(&lowerer->allocator, "%.*s",
                                  (int)(node->name.length >= 2 ? node->name.length - 2 : 0),
                                  node->name.text + 1);
        return true;
    }
    if (node->kind == TB_SHADER_NODE_NAME)
    {
        out->kind = TB_SHADER_FORM_ARGUMENT_NAME;
        out->text = name_text(lowerer, node->name);
        return true;
    }
    return false;
}

/* `[shader("pixel")]`: the stage a function is an entry point of. */
static void declare_entry_point(tb_shader_lowerer *lowerer, uint32_t function,
                                const tb_shader_form_attribute *attribute,
                                tb_shader_location location)
{
    tb_shader_form_stage stage = TB_SHADER_FORM_STAGE_VERTEX;
    const bool named = attribute->argument_count == 1 &&
                       attribute->arguments[0].kind == TB_SHADER_FORM_ARGUMENT_STRING &&
                       tb_shader_form_stage_named(attribute->arguments[0].text, &stage);
    if (!named)
    {
        tb_shader_lower_unsupported(lowerer, location, "a shader attribute that names no stage");
        return;
    }
    (void)tb_shader_form_add_entry_point(lowerer->form, function, stage);
}

/* A function's attributes, as written; a `shader` attribute makes it an
 * entry point. */
static void read_attributes(tb_shader_lowerer *lowerer, uint32_t index, const tb_shader_node *node)
{
    tb_shader_form_function *const function = tb_shader_form_function_at(lowerer->form, index);
    for (const tb_shader_attribute *source = node->attributes; source != NULL;
         source = source->next)
    {
        tb_shader_form_attribute attribute = {0};
        attribute.name = name_text(lowerer, source->name);
        attribute.arguments =
            TB_ARENA_NEW_ARRAY(lowerer->arena, tb_shader_form_argument, source->argument_count + 1);
        for (const tb_shader_node *argument = source->arguments; argument != NULL;
             argument = argument->next)
        {
            if (!attribute_argument(lowerer, argument,
                                    &attribute.arguments[attribute.argument_count]))
            {
                tb_shader_lower_unsupported(lowerer, argument->location,
                                            "an attribute's argument that is not a literal");
                return;
            }
            attribute.argument_count++;
        }
        (void)tb_array_push(function->attributes, &attribute);
        if (tb_str_equal(attribute.name, "shader"))
        {
            declare_entry_point(lowerer, index, &attribute, source->location);
        }
    }
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
    (void)tb_array_push(lowerer->function_locations, &node->location);
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
        entry.semantic = semantic_of(lowerer, parameter->annotations);
        entry.modifiers = parameter->modifiers & TB_SHADER_FORM_INTERFACE_MODIFIERS;
        if (entry.type == TB_SHADER_FORM_NONE)
        {
            return;
        }
        (void)tb_shader_form_add_parameter(function, &entry);
    }
    function->result_semantic = semantic_of(lowerer, node->annotations);
    tb_shader_lower_bind(lowerer, lowerer->functions, node->symbol, index);
    read_attributes(lowerer, index, node);
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
                                 tb_shader_lower_binding(TB_SHADER_VARIABLE_ARGUMENT, index));
            continue;
        }
        const uint32_t local =
            tb_shader_form_add_local(lowerer->function, entry->name, entry->type);
        tb_shader_lower_bind(lowerer, lowerer->bindings, parameter->symbol,
                             tb_shader_lower_binding(TB_SHADER_VARIABLE_LOCAL, local));
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

/* `register(t3, space1)`: where the source binds a buffer or a resource. */
static void read_register(const tb_shader_annotation *annotation, tb_shader_form_binding *binding)
{
    for (; annotation != NULL; annotation = annotation->next)
    {
        const bool is_register =
            annotation->name.length == 8 && memcmp(annotation->name.text, "register", 8) == 0;
        const tb_shader_node *const slot = annotation->arguments;
        if (!is_register || slot == NULL || slot->kind != TB_SHADER_NODE_NAME ||
            slot->name.length < 2)
        {
            continue;
        }
        binding->is_explicit = true;
        binding->register_class = (char)tolower((unsigned char)slot->name.text[0]);
        binding->index = (uint32_t)strtoul(slot->name.text + 1, NULL, 10);
        const tb_shader_node *const space = slot->next;
        const bool has_space = space != NULL && space->kind == TB_SHADER_NODE_NAME &&
                               space->name.length > 5 && memcmp(space->name.text, "space", 5) == 0;
        binding->space = has_space ? (uint32_t)strtoul(space->name.text + 5, NULL, 10) : 0;
        return;
    }
}

/* The register class of a resource: 's' for a sampler, 'u' for one that is
 * written, 't' for any other. */
static char register_class_of(const tb_shader_type *type)
{
    while (type->type_class == TB_SHADER_CLASS_ARRAY)
    {
        type = type->element;
    }
    if (type->object == TB_SHADER_OBJECT_SAMPLER ||
        type->object == TB_SHADER_OBJECT_SAMPLER_COMPARISON)
    {
        return 's';
    }
    if (type->object == TB_SHADER_OBJECT_CONSTANT_BUFFER)
    {
        return 'b';
    }
    const tb_shader_object_info *const info = tb_shader_object_info_of(type->object);
    return info != NULL && info->is_writable ? 'u' : 't';
}

static bool is_object_global(const tb_shader_type *type)
{
    while (type->type_class == TB_SHADER_CLASS_ARRAY)
    {
        type = type->element;
    }
    return type->type_class == TB_SHADER_CLASS_OBJECT;
}

/* A shader input declared alone: a resource, a ConstantBuffer<S>, or a loose
 * uniform, which joins `$Globals`. */
static void declare_input(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const tb_shader_symbol *const symbol = node->symbol;
    if (symbol->buffer != NULL)
    {
        return;
    }
    if (!is_object_global(symbol->type))
    {
        (void)tb_array_push(lowerer->loose, &node);
        return;
    }
    const bool is_buffer = symbol->type->type_class == TB_SHADER_CLASS_OBJECT &&
                           (symbol->type->object == TB_SHADER_OBJECT_CONSTANT_BUFFER ||
                            symbol->type->object == TB_SHADER_OBJECT_TEXTURE_BUFFER);
    if (symbol->type->type_class == TB_SHADER_CLASS_ARRAY && register_class_of(symbol->type) == 'b')
    {
        tb_shader_lower_unsupported(lowerer, node->location, "an array of constant buffers");
        return;
    }
    tb_shader_form_global global = {0};
    global.name = name_text(lowerer, symbol->name);
    global.type = tb_shader_lower_type(lowerer, symbol->type, node->location);
    global.space = is_buffer ? TB_SHADER_FORM_UNIFORM : TB_SHADER_FORM_HANDLE;
    global.binding.register_class = register_class_of(symbol->type);
    read_register(node->annotations, &global.binding);
    if (global.type == TB_SHADER_FORM_NONE)
    {
        return;
    }
    const uint32_t index = tb_shader_form_add_global(lowerer->form, &global);
    tb_shader_lower_bind(lowerer, lowerer->bindings, symbol,
                         tb_shader_lower_binding(TB_SHADER_VARIABLE_GLOBAL, index));
}

/* `packoffset(c1.y)`: the bytes from a constant buffer's start it places a
 * variable at, 20; NONE without one. */
static uint32_t packoffset_of(const tb_shader_annotation *annotation)
{
    for (; annotation != NULL; annotation = annotation->next)
    {
        const bool is_packoffset =
            annotation->name.length == 10 && memcmp(annotation->name.text, "packoffset", 10) == 0;
        const tb_shader_node *slot = annotation->arguments;
        if (!is_packoffset || slot == NULL)
        {
            continue;
        }
        uint32_t component = 0;
        if (slot->kind == TB_SHADER_NODE_MEMBER && slot->name.length == 1)
        {
            const char *const letters = "xyzw";
            for (uint32_t at = 0; at < 4; at++)
            {
                component = letters[at] == slot->name.text[0] ? at : component;
            }
            slot = slot->left;
        }
        if (slot->kind != TB_SHADER_NODE_NAME || slot->name.length < 2 || slot->name.text[0] != 'c')
        {
            continue;
        }
        const uint32_t row = (uint32_t)strtoul(slot->name.text + 1, NULL, 10);
        return row * 16 + component * 4;
    }
    return TB_SHADER_FORM_NONE;
}

/* A constant buffer of variables: a uniform global whose type is the
 * struct of its variables, each bound to its member. */
static void declare_buffer(tb_shader_lowerer *lowerer, const char *name,
                           tb_shader_node *const *variables, size_t count, char register_class,
                           const tb_shader_annotation *annotations, tb_shader_location location)
{
    tb_shader_form_member *const members =
        TB_ARENA_NEW_ARRAY(lowerer->arena, tb_shader_form_member, count + 1);
    for (size_t index = 0; index < count; index++)
    {
        members[index].name = name_text(lowerer, variables[index]->name);
        members[index].modifiers = layout_of(variables[index]);
        members[index].packoffset = packoffset_of(variables[index]->annotations);
        members[index].type = tb_shader_lower_type(lowerer, variables[index]->symbol->type,
                                                   variables[index]->location);
        if (members[index].type == TB_SHADER_FORM_NONE)
        {
            return;
        }
    }
    tb_shader_form_global global = {0};
    global.name = name;
    global.type = tb_shader_form_struct(lowerer->form, name, members, (uint32_t)count);
    global.space = TB_SHADER_FORM_UNIFORM;
    global.binding.register_class = register_class;
    read_register(annotations, &global.binding);
    const uint32_t index = tb_shader_form_add_global(lowerer->form, &global);
    for (size_t member = 0; member < count; member++)
    {
        tb_shader_lower_bind(lowerer, lowerer->members, variables[member]->symbol,
                             tb_shader_lower_member_binding(index, (uint32_t)member));
    }
    (void)location;
}

static void declare_cbuffer(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    size_t count = 0;
    for (tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        count += child->kind == TB_SHADER_NODE_VARIABLE && child->symbol != NULL ? 1u : 0u;
    }
    tb_shader_node **const variables =
        TB_ARENA_NEW_ARRAY(lowerer->arena, tb_shader_node *, count + 1);
    size_t at = 0;
    for (tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        if (child->kind == TB_SHADER_NODE_VARIABLE && child->symbol != NULL)
        {
            variables[at++] = child;
        }
    }
    const char register_class = node->kind == TB_SHADER_NODE_TBUFFER ? 't' : 'b';
    declare_buffer(lowerer, name_text(lowerer, node->name), variables, count, register_class,
                   node->annotations, node->location);
}

/* Loose uniforms, declared alone at namespace scope, are the members of a
 * constant buffer of their own, as DXC's `$Globals` is. */
static void declare_loose_uniforms(tb_shader_lowerer *lowerer)
{
    const size_t count = tb_array_length(lowerer->loose);
    if (count > 0)
    {
        tb_shader_location nowhere = {0};
        declare_buffer(lowerer, "$Globals", TB_ARRAY_DATA(lowerer->loose, tb_shader_node *), count,
                       'b', NULL, nowhere);
    }
}

static void declare_global(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const tb_shader_symbol *const symbol = node->symbol;
    if (symbol == NULL || symbol->has_value)
    {
        return;
    }
    if (symbol->storage == TB_SHADER_STORAGE_UNIFORM)
    {
        declare_input(lowerer, node);
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
                         tb_shader_lower_binding(TB_SHADER_VARIABLE_GLOBAL, index));
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
        else if ((node->kind == TB_SHADER_NODE_CBUFFER || node->kind == TB_SHADER_NODE_TBUFFER) &&
                 !bodies)
        {
            declare_cbuffer(lowerer, node);
        }
    }
}

/* -------------------------------------------------------------------------
 * Recursion, which HLSL does not allow.
 * ------------------------------------------------------------------------- */

typedef struct visit
{
    uint32_t function;
    uint32_t next_statement;
} visit;

/* The function the next call of a function's at or after `*at` calls, or
 * NONE when no call is left. */
static uint32_t next_callee(const tb_shader_form_function *function, uint32_t *at)
{
    const size_t count = tb_shader_form_statement_count(function);
    while (*at < count)
    {
        const tb_shader_form_statement *const statement =
            tb_shader_form_statement_at(function, (*at)++);
        if (statement->kind == TB_SHADER_FORM_CALL && statement->function != TB_SHADER_FORM_NONE)
        {
            return statement->function;
        }
    }
    return TB_SHADER_FORM_NONE;
}

/* Walk the calls from each function, depth first; a call of a function
 * still being walked is recursion, reported once, where that function is
 * declared. */
static void report_recursion(tb_shader_lowerer *lowerer)
{
    enum
    {
        UNSEEN,
        WALKING,
        DONE
    };
    const size_t count = tb_shader_form_function_count(lowerer->form);
    uint8_t *const states = TB_ARENA_NEW_ARRAY(lowerer->arena, uint8_t, count + 1);
    bool *const reported = TB_ARENA_NEW_ARRAY(lowerer->arena, bool, count + 1);
    visit *const stack = TB_ARENA_NEW_ARRAY(lowerer->arena, visit, count + 1);
    for (size_t root = 0; root < count; root++)
    {
        if (states[root] != UNSEEN)
        {
            continue;
        }
        size_t depth = 0;
        stack[depth++] = (visit){(uint32_t)root, 0};
        states[root] = WALKING;
        while (depth > 0)
        {
            visit *const top = &stack[depth - 1];
            const uint32_t callee = next_callee(
                tb_shader_form_function_at(lowerer->form, top->function), &top->next_statement);
            if (callee == TB_SHADER_FORM_NONE)
            {
                states[top->function] = DONE;
                depth--;
                continue;
            }
            if (states[callee] == WALKING && !reported[callee] &&
                callee < tb_array_length(lowerer->function_locations))
            {
                reported[callee] = true;
                tb_shader_lower_error(
                    lowerer, *TB_ARRAY_AT(lowerer->function_locations, tb_shader_location, callee),
                    tb_fmt_string(&lowerer->allocator,
                                  "'%s' calls itself, directly or through other functions, "
                                  "which HLSL does not allow",
                                  tb_shader_form_function_at(lowerer->form, callee)->name));
            }
            if (states[callee] == UNSEEN)
            {
                states[callee] = WALKING;
                stack[depth++] = (visit){callee, 0};
            }
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
    lowerer.members = new_map(&lowerer);
    lowerer.loose = TB_ARRAY_CREATE(&lowerer.allocator, tb_shader_node *);
    lowerer.function_locations = TB_ARRAY_CREATE(&lowerer.allocator, tb_shader_location);
    lower_declarations(&lowerer, syntax->declarations, false);
    declare_loose_uniforms(&lowerer);
    lower_declarations(&lowerer, syntax->declarations, true);
    lower_initializer_function(&lowerer);
    report_recursion(&lowerer);
    lowered->diagnostics = tb_array_data(lowerer.diagnostics);
    lowered->diagnostic_count = tb_array_length(lowerer.diagnostics);
    lowered->error_count = lowerer.error_count;
    return lowerer.error_count == 0 ? TB_SHADER_OK : TB_SHADER_FAILED;
}
