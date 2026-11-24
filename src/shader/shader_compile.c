/* Compiling: a source read, parsed, checked and lowered, then the part of
 * its form one entry point runs, and what that entry point takes, gives and
 * binds. Every step works in an arena of its own; the output is copied into
 * the caller's arena at the end, so only that copy can run out of room. */
#include "shader_check.h"
#include "shader_dxil.h"
#include "shader_form.h"
#include "shader_internal.h"
#include "shader_lower.h"
#include "shader_reflect.h"
#include "shader_syntax.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/assert.h"
#include "badger/dxc.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/str.h"

#include <string.h>

#define STAGE_COUNT ((size_t)TB_SHADER_STAGE_CALLABLE + 1)

/* The public stages are the form's, in its order. */
TB_STATIC_ASSERT(STAGE_COUNT == (size_t)TB_SHADER_FORM_STAGE_COUNT, "the stages differ");

typedef struct compile
{
    tb_arena *scratch;
    tb_allocator allocator;
    tb_array *diagnostics; /* tb_shader_diagnostic */
    size_t error_count;
    bool is_invalid;     /* The request, not the source, is wrong. */
    tb_shader_unit unit; /* The preprocessed source, once read. */
    const void *code;    /* What the target runs, in `scratch`. */
    size_t code_size;
} compile;

static void add_diagnostics(compile *state, const tb_shader_diagnostic *diagnostics, size_t count)
{
    tb_array_append(state->diagnostics, diagnostics, count);
    for (size_t index = 0; index < count; index++)
    {
        state->error_count += diagnostics[index].severity == TB_SHADER_ERROR ? 1u : 0u;
    }
}

/* An error that belongs to no place in a file. */
static void add_error(compile *state, const char *message)
{
    const tb_shader_diagnostic diagnostic = {TB_SHADER_ERROR, "", 0, 0, message};
    add_diagnostics(state, &diagnostic, 1);
}

/* -------------------------------------------------------------------------
 * The output, in the caller's arena.
 * ------------------------------------------------------------------------- */

static const char *copy_string(tb_arena *arena, const char *string)
{
    const size_t length = tb_str_length(string);
    char *const copy = tb_arena_try_alloc(arena, length + 1, 1);
    if (copy != NULL)
    {
        memcpy(copy, string, length + 1);
    }
    return copy;
}

static void *copy_items(tb_arena *arena, const void *items, size_t count, size_t size,
                        size_t alignment)
{
    void *const copy = tb_arena_try_calloc(arena, count == 0 ? 1 : count, size, alignment);
    if (copy != NULL && count > 0)
    {
        memcpy(copy, items, count * size);
    }
    return copy;
}

static bool copy_signature(tb_arena *arena, const tb_shader_signature_element *elements,
                           size_t count, const tb_shader_signature_element **out)
{
    tb_shader_signature_element *const copy = copy_items(arena, elements, count, sizeof(*elements),
                                                         _Alignof(tb_shader_signature_element));
    if (copy == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        copy[index].semantic = copy_string(arena, elements[index].semantic);
        if (copy[index].semantic == NULL)
        {
            return false;
        }
    }
    *out = copy;
    return true;
}

static bool copy_buffers(tb_arena *arena, const tb_shader_reflection *reflection,
                         tb_shader_reflection *out)
{
    tb_shader_constant_buffer *const buffers =
        copy_items(arena, reflection->buffers, reflection->buffer_count,
                   sizeof(tb_shader_constant_buffer), _Alignof(tb_shader_constant_buffer));
    if (buffers == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < reflection->buffer_count; index++)
    {
        const tb_shader_constant_buffer *const source = &reflection->buffers[index];
        tb_shader_variable *const variables =
            copy_items(arena, source->variables, source->variable_count, sizeof(tb_shader_variable),
                       _Alignof(tb_shader_variable));
        buffers[index].name = copy_string(arena, source->name);
        if (variables == NULL || buffers[index].name == NULL)
        {
            return false;
        }
        for (size_t at = 0; at < source->variable_count; at++)
        {
            variables[at].name = copy_string(arena, source->variables[at].name);
            if (variables[at].name == NULL)
            {
                return false;
            }
        }
        buffers[index].variables = variables;
    }
    out->buffers = buffers;
    return true;
}

static bool copy_reflection(tb_arena *arena, const tb_shader_reflection *reflection,
                            tb_shader_reflection *out)
{
    *out = *reflection;
    if (!copy_signature(arena, reflection->inputs, reflection->input_count, &out->inputs) ||
        !copy_signature(arena, reflection->outputs, reflection->output_count, &out->outputs) ||
        !copy_signature(arena, reflection->patch_constants, reflection->patch_constant_count,
                        &out->patch_constants))
    {
        return false;
    }
    tb_shader_binding *const bindings =
        copy_items(arena, reflection->bindings, reflection->binding_count,
                   sizeof(tb_shader_binding), _Alignof(tb_shader_binding));
    if (bindings == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < reflection->binding_count; index++)
    {
        bindings[index].name = copy_string(arena, reflection->bindings[index].name);
        if (bindings[index].name == NULL)
        {
            return false;
        }
    }
    out->bindings = bindings;
    return copy_buffers(arena, reflection, out);
}

static bool copy_output(const compile *state, const tb_shader_reflection *reflection,
                        tb_arena *arena, tb_shader_compiled *output)
{
    const size_t count = tb_array_length(state->diagnostics);
    const tb_shader_diagnostic *const source = tb_array_data(state->diagnostics);
    tb_shader_diagnostic *const diagnostics = copy_items(
        arena, source, count, sizeof(tb_shader_diagnostic), _Alignof(tb_shader_diagnostic));
    if (diagnostics == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        diagnostics[index].path = copy_string(arena, source[index].path);
        diagnostics[index].message = copy_string(arena, source[index].message);
        if (diagnostics[index].path == NULL || diagnostics[index].message == NULL)
        {
            return false;
        }
    }
    output->diagnostics = diagnostics;
    output->diagnostic_count = count;
    if (reflection == NULL)
    {
        return true;
    }
    if (state->code_size > 0)
    {
        /* A DXBC container is made of 32-bit words. */
        output->code = copy_items(arena, state->code, state->code_size, 1, sizeof(uint32_t));
        output->code_size = state->code_size;
        if (output->code == NULL)
        {
            return false;
        }
    }
    return copy_reflection(arena, reflection, &output->reflection);
}

/* -------------------------------------------------------------------------
 * The steps.
 * ------------------------------------------------------------------------- */

/* The form's function an entry point names: the first of that name that is
 * not a method; NONE when there is none. */
static uint32_t find_entry_function(const tb_shader_form *form, const char *name)
{
    const size_t count = tb_shader_form_function_count(form);
    for (size_t index = 0; index < count; index++)
    {
        if (tb_str_equal(tb_shader_form_function_at(form, (uint32_t)index)->name, name))
        {
            return (uint32_t)index;
        }
    }
    return TB_SHADER_FORM_NONE;
}

static uint32_t entry_point_index(const tb_shader_form *form, uint32_t function,
                                  tb_shader_form_stage stage)
{
    const size_t count = tb_array_length(form->entry_points);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_entry_point *const entry =
            TB_ARRAY_AT(form->entry_points, tb_shader_form_entry_point, index);
        if (entry->function == function && entry->stage == stage)
        {
            return (uint32_t)index;
        }
    }
    return TB_SHADER_FORM_NONE;
}

/* Lower the source, then reflect the entry point; false after an error. */
static bool reflect_entry(compile *state, const tb_shader_compile_request *request,
                          tb_shader_reflection *reflection)
{
    tb_shader_unit *const unit = &state->unit;
    const tb_shader_result preprocessed =
        tb_shader_preprocess_unit(&request->source, state->scratch, unit);
    if (preprocessed != TB_SHADER_OK)
    {
        add_diagnostics(state, unit->diagnostics, unit->diagnostic_count);
        state->is_invalid = preprocessed == TB_SHADER_INVALID;
        return false;
    }
    tb_shader_syntax syntax = {0};
    (void)tb_shader_parse(unit, state->scratch, &syntax);
    tb_shader_checked checked = {0};
    const tb_shader_result result = tb_shader_check(&syntax, state->scratch, &checked);
    add_diagnostics(state, checked.diagnostics, checked.diagnostic_count);
    if (result != TB_SHADER_OK)
    {
        return false;
    }
    tb_shader_form form = {0};
    tb_shader_lowered lowered = {0};
    const tb_shader_result lowering =
        tb_shader_lower(&syntax, &checked, state->scratch, &form, &lowered);
    add_diagnostics(state, lowered.diagnostics, lowered.diagnostic_count);
    if (lowering != TB_SHADER_OK)
    {
        return false;
    }
    const uint32_t function = find_entry_function(&form, request->entry_point);
    if (function == TB_SHADER_FORM_NONE)
    {
        add_error(state, tb_fmt_string(&state->allocator, "there is no function '%s' to run",
                                       request->entry_point));
        return false;
    }
    const tb_shader_form_stage stage = (tb_shader_form_stage)request->stage;
    (void)tb_shader_form_add_entry_point(&form, function, stage);
    tb_shader_form entry = {0};
    const bool extracted = tb_shader_form_extract(&form, entry_point_index(&form, function, stage),
                                                  state->scratch, &entry);
    TB_ASSERT(extracted);
    const char *const why = tb_shader_reflect(&entry, state->scratch, reflection);
    if (why != NULL)
    {
        add_error(state, tb_fmt_string(&state->allocator, "entry point '%s': %s",
                                       request->entry_point, why));
        return false;
    }
    return extracted;
}

/* -------------------------------------------------------------------------
 * Writing for a target.
 * ------------------------------------------------------------------------- */

/* The request's shader model, its default applied. */
static unsigned shader_model_of(const tb_shader_compile_request *request)
{
    return request->shader_model == 0 ? TB_SHADER_MODEL_DEFAULT : request->shader_model;
}

/* What is wrong with the request's target, or NULL when nothing is. */
static const char *check_target(const tb_shader_compile_request *request)
{
    if (request->target == TB_SHADER_TARGET_NONE)
    {
        return NULL;
    }
    if (request->target != TB_SHADER_TARGET_DXIL)
    {
        return "a compile's target is one of tb_shader_target";
    }
    if (request->dxc == NULL)
    {
        return "a compile for DXIL needs DXC";
    }
    if ((shader_model_of(request) >> 8) != 6)
    {
        return "DXIL is written for shader model 6";
    }
    return NULL;
}

/* Compile the preprocessed source into DXIL; false after an error. DXC
 * reads the text without `#line`, so each place it names is a place in the
 * text, which the tokens' offsets lead back to the sources. */
static bool write_dxil(compile *state, const tb_shader_compile_request *request)
{
    tb_array *const text = TB_ARRAY_CREATE(&state->allocator, char);
    tb_array *const offsets = TB_ARRAY_CREATE(&state->allocator, size_t);
    tb_shader_render(&state->unit, false, text, offsets);
    char profile[TB_SHADER_PROFILE_CAPACITY] = {0};
    tb_shader_dxil_profile(request->stage, shader_model_of(request), profile);
    tb_dxc_request dxc_request = {0};
    dxc_request.source = tb_array_data(text);
    dxc_request.source_length = tb_array_length(text);
    /* The main path without its first `/`, which DXC would read as an option. */
    dxc_request.name = request->source.path + 1;
    dxc_request.profile = profile;
    dxc_request.entry_point = request->entry_point;
    dxc_request.flags = (request->flags & TB_SHADER_COMPILE_DEBUG) != 0 ? TB_DXC_DEBUG : 0u;
    tb_dxc_output output = {0};
    tb_dxc_error error = TB_DXC_ERROR_INIT;
    const tb_dxc_result result = tb_dxc_compile(request->dxc, &dxc_request, &output, &error);
    tb_shader_rendered rendered = {0};
    rendered.unit = &state->unit;
    rendered.text = dxc_request.source;
    rendered.length = dxc_request.source_length;
    rendered.offsets = tb_array_data(offsets);
    const size_t errors =
        tb_shader_dxc_diagnostics(output.messages, output.messages_length, dxc_request.name,
                                  &rendered, state->scratch, state->diagnostics);
    state->error_count += errors;
    if (result == TB_DXC_OK)
    {
        void *const code = tb_arena_alloc(state->scratch, output.code_size, sizeof(uint32_t));
        memcpy(code, output.code, output.code_size);
        state->code = code;
        state->code_size = output.code_size;
    }
    else if (errors == 0)
    {
        /* coverage-off: DXC says in its messages why a compile failed */
        add_error(state, tb_fmt_string(&state->allocator, "dxc: %s", error.message));
        /* coverage-on */
    }
    tb_dxc_release(&output);
    return result == TB_DXC_OK;
}

tb_shader_result tb_shader_compile(const tb_shader_compile_request *request, tb_arena *arena,
                                   tb_shader_compiled *output)
{
    const tb_shader_compiled empty = {0};
    *output = empty;
    compile state = {0};
    state.scratch = tb_arena_create(0);
    state.allocator = tb_arena_allocator(state.scratch);
    state.diagnostics = TB_ARRAY_CREATE(&state.allocator, tb_shader_diagnostic);
    tb_shader_result result = TB_SHADER_OK;
    tb_shader_reflection reflection = {0};
    const char *const target_problem = check_target(request);
    if (request->entry_point == NULL || request->entry_point[0] == '\0' ||
        (size_t)request->stage >= STAGE_COUNT)
    {
        add_error(&state, "a compile names an entry point and a stage");
        result = TB_SHADER_INVALID;
    }
    else if (target_problem != NULL)
    {
        add_error(&state, target_problem);
        result = TB_SHADER_INVALID;
    }
    else if (!reflect_entry(&state, request, &reflection))
    {
        result = state.is_invalid ? TB_SHADER_INVALID : TB_SHADER_FAILED;
    }
    else if (request->target == TB_SHADER_TARGET_DXIL && !write_dxil(&state, request))
    {
        result = TB_SHADER_FAILED;
    }
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    if (!copy_output(&state, result == TB_SHADER_OK ? &reflection : NULL, arena, output))
    {
        tb_arena_rewind(arena, mark);
        *output = empty;
        result = TB_SHADER_NO_MEMORY;
    }
    tb_arena_destroy(state.scratch);
    return result;
}

const char *tb_shader_stage_name(tb_shader_stage stage)
{
    return (size_t)stage < STAGE_COUNT ? tb_shader_form_stage_name((tb_shader_form_stage)stage)
                                       : "unknown stage";
}

const char *tb_shader_feature_name(tb_shader_feature feature)
{
    switch (feature)
    {
    case TB_SHADER_FEATURE_WAVE_OPERATIONS:
        return "wave operations";
    case TB_SHADER_FEATURE_BINDLESS:
        return "bindless resources";
    case TB_SHADER_FEATURE_FLOAT64:
        return "64-bit floats";
    case TB_SHADER_FEATURE_INT64:
        return "64-bit integers";
    case TB_SHADER_FEATURE_ATOMIC64:
        return "64-bit atomics";
    case TB_SHADER_FEATURE_RAY_QUERY:
        return "ray queries";
    case TB_SHADER_FEATURE_RAY_TRACING:
        return "ray-tracing pipelines";
    case TB_SHADER_FEATURE_MESH_SHADER:
        return "mesh shaders";
    case TB_SHADER_FEATURE_SAMPLER_FEEDBACK:
        return "sampler feedback";
    }
    return "unknown feature";
}
