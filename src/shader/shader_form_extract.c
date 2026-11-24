/* The part of a form one entry point runs: the functions it calls (and a
 * hull shader's patch-constant function), the
 * globals they use, and the initializer when it sets one of those globals,
 * copied into a form of its own. Reflection describes that form, and each
 * backend writes it, so what an entry point does not use is neither bound
 * nor written, as DXC drops it. */
#include "shader_form.h"

#include "badger/arena.h"
#include "badger/assert.h"

typedef struct reach
{
    const tb_shader_form *form;
    bool *functions;
    bool *globals;
    uint32_t *pending; /* Functions found, to be scanned. */
    size_t pending_count;
} reach;

static void reach_function(reach *state, uint32_t function)
{
    if (!state->functions[function])
    {
        state->functions[function] = true;
        state->pending[state->pending_count++] = function;
    }
}

/* Mark what the functions found so far use, and the functions they call. */
static void scan(reach *state)
{
    while (state->pending_count > 0)
    {
        const tb_shader_form_function *const function =
            tb_shader_form_function_at(state->form, state->pending[--state->pending_count]);
        const size_t expression_count = tb_shader_form_expression_count(function);
        for (size_t index = 0; index < expression_count; index++)
        {
            const tb_shader_form_expression *const expression =
                tb_shader_form_expression_at(function, (uint32_t)index);
            if (expression->kind == TB_SHADER_FORM_GLOBAL)
            {
                state->globals[expression->index] = true;
            }
        }
        const size_t statement_count = tb_shader_form_statement_count(function);
        for (size_t index = 0; index < statement_count; index++)
        {
            const tb_shader_form_statement *const statement =
                tb_shader_form_statement_at(function, (uint32_t)index);
            if (statement->kind == TB_SHADER_FORM_CALL &&
                statement->function != TB_SHADER_FORM_NONE)
            {
                reach_function(state, statement->function);
            }
        }
    }
}

/* Whether the initializer stores to a global the entry point uses. */
static bool initializes_reached(const reach *state, const tb_shader_form_function *initializer)
{
    const size_t statement_count = tb_shader_form_statement_count(initializer);
    for (size_t index = 0; index < statement_count; index++)
    {
        const tb_shader_form_statement *const statement =
            tb_shader_form_statement_at(initializer, (uint32_t)index);
        if (statement->kind != TB_SHADER_FORM_STORE)
        {
            continue;
        }
        const tb_shader_form_expression *target =
            tb_shader_form_expression_at(initializer, statement->operands[0]);
        while (target->kind == TB_SHADER_FORM_ACCESS || target->kind == TB_SHADER_FORM_ACCESS_INDEX)
        {
            target = tb_shader_form_expression_at(initializer, target->operands[0]);
        }
        if (target->kind == TB_SHADER_FORM_GLOBAL && state->globals[target->index])
        {
            return true;
        }
    }
    return false;
}

/* A copy of a function, its globals and the functions it calls renumbered. */
static tb_shader_form_function copy_function(const tb_shader_form_function *source,
                                             const tb_allocator *allocator,
                                             const uint32_t *global_numbers,
                                             const uint32_t *function_numbers)
{
    tb_shader_form_function copy = *source;
    copy.parameters = tb_array_copy(source->parameters, allocator);
    copy.locals = tb_array_copy(source->locals, allocator);
    copy.expressions = tb_array_copy(source->expressions, allocator);
    copy.operands = tb_array_copy(source->operands, allocator);
    copy.statements = tb_array_copy(source->statements, allocator);
    copy.attributes = tb_array_copy(source->attributes, allocator);
    const size_t expression_count = tb_array_length(copy.expressions);
    for (size_t index = 0; index < expression_count; index++)
    {
        tb_shader_form_expression *const expression =
            TB_ARRAY_AT(copy.expressions, tb_shader_form_expression, index);
        if (expression->kind == TB_SHADER_FORM_GLOBAL)
        {
            expression->index = global_numbers[expression->index];
        }
    }
    const size_t statement_count = tb_array_length(copy.statements);
    for (size_t index = 0; index < statement_count; index++)
    {
        tb_shader_form_statement *const statement =
            TB_ARRAY_AT(copy.statements, tb_shader_form_statement, index);
        if (statement->kind == TB_SHADER_FORM_CALL && statement->function != TB_SHADER_FORM_NONE)
        {
            statement->function = function_numbers[statement->function];
        }
    }
    return copy;
}

bool tb_shader_form_extract(const tb_shader_form *form, uint32_t entry_point, tb_arena *arena,
                            tb_shader_form *out)
{
    tb_shader_form_init(out, arena);
    if (entry_point >= tb_array_length(form->entry_points))
    {
        return false;
    }
    const tb_shader_form_entry_point *const entry =
        TB_ARRAY_AT(form->entry_points, tb_shader_form_entry_point, entry_point);
    const size_t function_count = tb_shader_form_function_count(form);
    const size_t global_count = tb_array_length(form->globals);
    reach state = {0};
    state.form = form;
    state.functions = TB_ARENA_NEW_ARRAY(arena, bool, function_count + 1);
    state.globals = TB_ARENA_NEW_ARRAY(arena, bool, global_count + 1);
    state.pending = TB_ARENA_NEW_ARRAY(arena, uint32_t, function_count + 1);
    reach_function(&state, entry->function);
    /* A hull shader runs its patch-constant function too. */
    const uint32_t patch_constants =
        entry->stage == TB_SHADER_FORM_STAGE_HULL
            ? tb_shader_form_patch_constant_function(form, entry->function)
            : TB_SHADER_FORM_NONE;
    if (patch_constants != TB_SHADER_FORM_NONE)
    {
        reach_function(&state, patch_constants);
    }
    scan(&state);
    const bool keeps_initializer =
        form->initializer != TB_SHADER_FORM_NONE &&
        initializes_reached(&state, tb_shader_form_function_at(form, form->initializer));
    if (keeps_initializer)
    {
        reach_function(&state, form->initializer);
        scan(&state);
    }

    /* Types, their members and the strings are kept whole, so their indices
     * hold; globals and functions are numbered anew. */
    tb_array_destroy(out->types);
    tb_array_destroy(out->members);
    tb_array_destroy(out->strings);
    out->types = tb_array_copy(form->types, &out->allocator);
    out->members = tb_array_copy(form->members, &out->allocator);
    out->strings = tb_array_copy(form->strings, &out->allocator);
    uint32_t *const global_numbers = TB_ARENA_NEW_ARRAY(arena, uint32_t, global_count + 1);
    for (size_t index = 0; index < global_count; index++)
    {
        global_numbers[index] = TB_SHADER_FORM_NONE;
        if (state.globals[index])
        {
            global_numbers[index] = tb_shader_form_add_global(
                out, TB_ARRAY_AT(form->globals, tb_shader_form_global, index));
        }
    }
    uint32_t *const function_numbers = TB_ARENA_NEW_ARRAY(arena, uint32_t, function_count + 1);
    uint32_t kept = 0;
    for (size_t index = 0; index < function_count; index++)
    {
        function_numbers[index] = state.functions[index] ? kept++ : TB_SHADER_FORM_NONE;
    }
    for (size_t index = 0; index < function_count; index++)
    {
        if (!state.functions[index])
        {
            continue;
        }
        const tb_shader_form_function copy =
            copy_function(tb_shader_form_function_at(form, (uint32_t)index), &out->allocator,
                          global_numbers, function_numbers);
        (void)tb_array_push(out->functions, &copy);
    }
    out->initializer =
        keeps_initializer ? function_numbers[form->initializer] : TB_SHADER_FORM_NONE;
    (void)tb_shader_form_add_entry_point(out, function_numbers[entry->function], entry->stage);
    return true;
}
