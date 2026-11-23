/* Making a form: its types, interned, and the parts of its functions. */
#include "shader_form.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/str.h"

static const char *scalar_name(tb_shader_scalar scalar)
{
    static const struct
    {
        tb_shader_scalar scalar;
        const char *name;
    } names[] = {
        {TB_SHADER_SCALAR_BOOL, "bool"},           {TB_SHADER_SCALAR_INT, "int"},
        {TB_SHADER_SCALAR_UINT, "uint"},           {TB_SHADER_SCALAR_INT16, "int16_t"},
        {TB_SHADER_SCALAR_UINT16, "uint16_t"},     {TB_SHADER_SCALAR_INT64, "int64_t"},
        {TB_SHADER_SCALAR_UINT64, "uint64_t"},     {TB_SHADER_SCALAR_MIN16INT, "min16int"},
        {TB_SHADER_SCALAR_MIN16UINT, "min16uint"}, {TB_SHADER_SCALAR_MIN16FLOAT, "min16float"},
        {TB_SHADER_SCALAR_HALF, "half"},           {TB_SHADER_SCALAR_FLOAT, "float"},
        {TB_SHADER_SCALAR_DOUBLE, "double"},
    };
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    {
        if (names[index].scalar == scalar)
        {
            return names[index].name;
        }
    }
    return "(literal)";
}

void tb_shader_form_init(tb_shader_form *form, tb_arena *arena)
{
    const tb_shader_form empty = {0};
    *form = empty;
    form->arena = arena;
    form->allocator = tb_arena_allocator(arena);
    form->types = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_type);
    form->members = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_member);
    form->globals = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_global);
    form->functions = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_function);
    form->strings = TB_ARRAY_CREATE(&form->allocator, const char *);
    form->initializer = TB_SHADER_FORM_NONE;
    form->entry_points = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_entry_point);
}

static const char *const stage_names[TB_SHADER_FORM_STAGE_COUNT] = {
    "vertex", "pixel",         "geometry",     "hull",   "domain",     "compute", "amplification",
    "mesh",   "raygeneration", "intersection", "anyhit", "closesthit", "miss",    "callable",
};

const char *tb_shader_form_stage_name(tb_shader_form_stage stage)
{
    return (size_t)stage < TB_SHADER_FORM_STAGE_COUNT ? stage_names[stage] : "?";
}

bool tb_shader_form_stage_named(const char *name, tb_shader_form_stage *stage)
{
    for (size_t index = 0; index < TB_SHADER_FORM_STAGE_COUNT; index++)
    {
        if (tb_str_equal(name, stage_names[index]))
        {
            *stage = (tb_shader_form_stage)index;
            return true;
        }
    }
    return false;
}

bool tb_shader_form_add_entry_point(tb_shader_form *form, uint32_t function,
                                    tb_shader_form_stage stage)
{
    const size_t count = tb_array_length(form->entry_points);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_entry_point *const entry =
            TB_ARRAY_AT(form->entry_points, tb_shader_form_entry_point, index);
        if (entry->function == function && entry->stage == stage)
        {
            return false;
        }
    }
    const tb_shader_form_entry_point entry = {function, stage};
    (void)tb_array_push(form->entry_points, &entry);
    return true;
}

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

static bool same_type(const tb_shader_form_type *left, const tb_shader_form_type *right)
{
    return left->kind == right->kind && left->scalar == right->scalar &&
           left->rows == right->rows && left->columns == right->columns &&
           left->element == right->element && left->length == right->length &&
           left->second_length == right->second_length && left->object == right->object;
}

/* The index of a type like `probe`, added when there is none; never a struct. */
static uint32_t intern(tb_shader_form *form, const tb_shader_form_type *probe)
{
    const size_t count = tb_array_length(form->types);
    const tb_shader_form_type *const types = TB_ARRAY_DATA(form->types, tb_shader_form_type);
    for (size_t index = 0; index < count; index++)
    {
        if (types[index].kind != TB_SHADER_FORM_STRUCT && same_type(&types[index], probe))
        {
            return (uint32_t)index;
        }
    }
    (void)tb_array_push(form->types, probe);
    return (uint32_t)count;
}

uint32_t tb_shader_form_void(tb_shader_form *form)
{
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_VOID;
    probe.name = "void";
    return intern(form, &probe);
}

uint32_t tb_shader_form_string(tb_shader_form *form)
{
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_STRING;
    probe.name = "string";
    return intern(form, &probe);
}

uint32_t tb_shader_form_resource(tb_shader_form *form, tb_shader_object object, uint32_t element,
                                 uint32_t length, uint32_t second_length, const char *name)
{
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_RESOURCE;
    probe.object = object;
    probe.element = element;
    probe.length = length;
    probe.second_length = second_length;
    probe.name = name;
    return intern(form, &probe);
}

uint32_t tb_shader_form_scalar(tb_shader_form *form, tb_shader_scalar scalar)
{
    TB_ASSERT(!tb_shader_scalar_is_literal(scalar));
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_SCALAR;
    probe.scalar = scalar;
    probe.rows = 1;
    probe.columns = 1;
    probe.name = scalar_name(scalar);
    return intern(form, &probe);
}

uint32_t tb_shader_form_vector(tb_shader_form *form, tb_shader_scalar scalar, uint32_t columns)
{
    TB_ASSERT(columns >= 1 && columns <= 4);
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_VECTOR;
    probe.scalar = scalar;
    probe.rows = 1;
    probe.columns = columns;
    const uint32_t found = intern(form, &probe);
    tb_shader_form_type *const type = TB_ARRAY_AT(form->types, tb_shader_form_type, found);
    if (type->name == NULL)
    {
        type->name =
            tb_fmt_string(&form->allocator, "%s%u", scalar_name(scalar), (unsigned)columns);
    }
    return found;
}

uint32_t tb_shader_form_matrix(tb_shader_form *form, tb_shader_scalar scalar, uint32_t rows,
                               uint32_t columns)
{
    TB_ASSERT(rows >= 1 && rows <= 4 && columns >= 1 && columns <= 4);
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_MATRIX;
    probe.scalar = scalar;
    probe.rows = rows;
    probe.columns = columns;
    const uint32_t found = intern(form, &probe);
    tb_shader_form_type *const type = TB_ARRAY_AT(form->types, tb_shader_form_type, found);
    if (type->name == NULL)
    {
        type->name = tb_fmt_string(&form->allocator, "%s%ux%u", scalar_name(scalar), (unsigned)rows,
                                   (unsigned)columns);
    }
    return found;
}

uint32_t tb_shader_form_shaped(tb_shader_form *form, tb_shader_scalar scalar, uint32_t shape)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(form, shape);
    if (type->kind == TB_SHADER_FORM_MATRIX)
    {
        return tb_shader_form_matrix(form, scalar, type->rows, type->columns);
    }
    if (type->kind == TB_SHADER_FORM_VECTOR)
    {
        return tb_shader_form_vector(form, scalar, type->columns);
    }
    return tb_shader_form_scalar(form, scalar);
}

uint32_t tb_shader_form_array(tb_shader_form *form, uint32_t element, uint32_t length)
{
    tb_shader_form_type probe = {0};
    probe.kind = TB_SHADER_FORM_ARRAY;
    probe.element = element;
    probe.length = length;
    const uint32_t found = intern(form, &probe);
    tb_shader_form_type *const type = TB_ARRAY_AT(form->types, tb_shader_form_type, found);
    if (type->name == NULL)
    {
        const char *const element_name = tb_shader_form_type_at(form, element)->name;
        type->name =
            length == 0 ? tb_fmt_string(&form->allocator, "%s[]", element_name)
                        : tb_fmt_string(&form->allocator, "%s[%u]", element_name, (unsigned)length);
    }
    return found;
}

uint32_t tb_shader_form_struct(tb_shader_form *form, const char *name,
                               const tb_shader_form_member *members, uint32_t count)
{
    tb_shader_form_type type = {0};
    type.kind = TB_SHADER_FORM_STRUCT;
    type.first_member = (uint32_t)tb_array_length(form->members);
    type.member_count = count;
    type.name = name;
    tb_array_append(form->members, members, count);
    const uint32_t index = (uint32_t)tb_array_length(form->types);
    (void)tb_array_push(form->types, &type);
    return index;
}

uint32_t tb_shader_form_find_numeric(const tb_shader_form *form, tb_shader_form_type_kind kind,
                                     tb_shader_scalar scalar, uint32_t rows, uint32_t columns)
{
    tb_shader_form_type probe = {0};
    probe.kind = kind;
    probe.scalar = scalar;
    probe.rows = rows;
    probe.columns = columns;
    const size_t count = tb_array_length(form->types);
    const tb_shader_form_type *const types = TB_ARRAY_DATA(form->types, tb_shader_form_type);
    for (size_t index = 0; index < count; index++)
    {
        if (same_type(&types[index], &probe))
        {
            return (uint32_t)index;
        }
    }
    return TB_SHADER_FORM_NONE;
}

uint32_t tb_shader_form_find_shaped(const tb_shader_form *form, tb_shader_scalar scalar,
                                    uint32_t shape)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(form, shape);
    return tb_shader_form_find_numeric(form, type->kind, scalar, type->rows, type->columns);
}

const tb_shader_form_type *tb_shader_form_type_at(const tb_shader_form *form, uint32_t type)
{
    TB_ASSERT(type < tb_array_length(form->types));
    return TB_ARRAY_AT(form->types, tb_shader_form_type, type);
}

const tb_shader_form_member *tb_shader_form_member_at(const tb_shader_form *form, uint32_t member)
{
    TB_ASSERT(member < tb_array_length(form->members));
    return TB_ARRAY_AT(form->members, tb_shader_form_member, member);
}

uint32_t tb_shader_form_component_count(const tb_shader_form *form, uint32_t type)
{
    const tb_shader_form_type *const found = tb_shader_form_type_at(form, type);
    switch (found->kind)
    {
    case TB_SHADER_FORM_SCALAR:
        return 1;
    case TB_SHADER_FORM_VECTOR:
        return found->columns;
    case TB_SHADER_FORM_MATRIX:
        return found->rows * found->columns;
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_ARRAY:
    case TB_SHADER_FORM_STRUCT:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        return 0;
    }
    return 0;
}

bool tb_shader_form_is_numeric(const tb_shader_form *form, uint32_t type)
{
    return tb_shader_form_component_count(form, type) > 0;
}

/* -------------------------------------------------------------------------
 * Variables and functions.
 * ------------------------------------------------------------------------- */

uint32_t tb_shader_form_add_global(tb_shader_form *form, const tb_shader_form_global *global)
{
    const uint32_t index = (uint32_t)tb_array_length(form->globals);
    (void)tb_array_push(form->globals, global);
    return index;
}

uint32_t tb_shader_form_add_string(tb_shader_form *form, const char *text, size_t length)
{
    const char *const copy = tb_fmt_string(&form->allocator, "%.*s", (int)length, text);
    const uint32_t index = (uint32_t)tb_array_length(form->strings);
    (void)tb_array_push(form->strings, &copy);
    return index;
}

const char *tb_shader_form_string_at(const tb_shader_form *form, uint32_t string)
{
    TB_ASSERT(string < tb_array_length(form->strings));
    return *TB_ARRAY_AT(form->strings, const char *, string);
}

uint32_t tb_shader_form_add_function(tb_shader_form *form, const char *name, uint32_t result)
{
    tb_shader_form_function function = {0};
    function.name = name;
    function.result = result;
    function.parameters = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_parameter);
    function.locals = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_local);
    function.expressions = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_expression);
    function.operands = TB_ARRAY_CREATE(&form->allocator, uint32_t);
    function.statements = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_statement);
    function.attributes = TB_ARRAY_CREATE(&form->allocator, tb_shader_form_attribute);
    tb_shader_form_statement body = {0};
    body.kind = TB_SHADER_FORM_BLOCK;
    body.next = TB_SHADER_FORM_NONE;
    body.first = TB_SHADER_FORM_NONE;
    function.body = tb_shader_form_add_statement(&function, &body);
    const uint32_t index = (uint32_t)tb_array_length(form->functions);
    (void)tb_array_push(form->functions, &function);
    return index;
}

tb_shader_form_function *tb_shader_form_function_at(const tb_shader_form *form, uint32_t function)
{
    TB_ASSERT(function < tb_array_length(form->functions));
    return TB_ARRAY_AT(form->functions, tb_shader_form_function, function);
}

size_t tb_shader_form_function_count(const tb_shader_form *form)
{
    return tb_array_length(form->functions);
}

uint32_t tb_shader_form_add_parameter(tb_shader_form_function *function,
                                      const tb_shader_form_parameter *parameter)
{
    const uint32_t index = (uint32_t)tb_array_length(function->parameters);
    (void)tb_array_push(function->parameters, parameter);
    return index;
}

uint32_t tb_shader_form_add_local(tb_shader_form_function *function, const char *name,
                                  uint32_t type)
{
    const tb_shader_form_local local = {name, type};
    const uint32_t index = (uint32_t)tb_array_length(function->locals);
    (void)tb_array_push(function->locals, &local);
    return index;
}

uint32_t tb_shader_form_add_expression(tb_shader_form_function *function,
                                       const tb_shader_form_expression *expression)
{
    const uint32_t index = (uint32_t)tb_array_length(function->expressions);
    (void)tb_array_push(function->expressions, expression);
    return index;
}

uint32_t tb_shader_form_add_statement(tb_shader_form_function *function,
                                      const tb_shader_form_statement *statement)
{
    const uint32_t index = (uint32_t)tb_array_length(function->statements);
    (void)tb_array_push(function->statements, statement);
    return index;
}

uint32_t tb_shader_form_add_operands(tb_shader_form_function *function, const uint32_t *operands,
                                     uint32_t count)
{
    const uint32_t start = (uint32_t)tb_array_length(function->operands);
    tb_array_append(function->operands, operands, count);
    return start;
}

tb_shader_form_expression *tb_shader_form_expression_at(const tb_shader_form_function *function,
                                                        uint32_t expression)
{
    TB_ASSERT(expression < tb_array_length(function->expressions));
    return TB_ARRAY_AT(function->expressions, tb_shader_form_expression, expression);
}

tb_shader_form_statement *tb_shader_form_statement_at(const tb_shader_form_function *function,
                                                      uint32_t statement)
{
    TB_ASSERT(statement < tb_array_length(function->statements));
    return TB_ARRAY_AT(function->statements, tb_shader_form_statement, statement);
}

uint32_t tb_shader_form_operand_at(const tb_shader_form_function *function, uint32_t at)
{
    TB_ASSERT(at < tb_array_length(function->operands));
    return *TB_ARRAY_AT(function->operands, uint32_t, at);
}

size_t tb_shader_form_expression_count(const tb_shader_form_function *function)
{
    return tb_array_length(function->expressions);
}

size_t tb_shader_form_statement_count(const tb_shader_form_function *function)
{
    return tb_array_length(function->statements);
}

bool tb_shader_form_is_emitted(tb_shader_form_expression_kind kind)
{
    switch (kind)
    {
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
        return false;
    case TB_SHADER_FORM_LOAD:
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_ACCESS_INDEX:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_CONVERT:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_TEXEL:
        return true;
    }
    return true;
}
