/* Reflection, read from the form of one entry point: its stage and thread
 * group, the values it takes and gives by their semantics, and the buffers,
 * textures and samplers it binds, with registers assigned as DXC assigns
 * them. */
#include "shader_reflect.h"

#include "badger/array.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct reflector
{
    const tb_shader_form *form;
    tb_arena *arena;
    tb_allocator allocator;
    tb_shader_stage stage;
    tb_array *inputs;   /* tb_shader_signature_element */
    tb_array *outputs;  /* tb_shader_signature_element */
    tb_array *bindings; /* tb_shader_binding */
    const char *error;  /* The first reason the entry point cannot run. */
} reflector;

static void fail(reflector *state, const char *format, const char *name)
{
    if (state->error == NULL)
    {
        state->error = tb_fmt_string(&state->allocator, format, name);
    }
}

static const tb_shader_form_type *type_at(const reflector *state, uint32_t type)
{
    return tb_shader_form_type_at(state->form, type);
}

/* -------------------------------------------------------------------------
 * Signatures.
 * ------------------------------------------------------------------------- */

static tb_shader_component component_of(tb_shader_scalar scalar)
{
    switch (scalar)
    {
    case TB_SHADER_SCALAR_INT:
    case TB_SHADER_SCALAR_LITERAL_INT:
        return TB_SHADER_COMPONENT_INT32;
    case TB_SHADER_SCALAR_BOOL:
    case TB_SHADER_SCALAR_UINT:
        return TB_SHADER_COMPONENT_UINT32;
    case TB_SHADER_SCALAR_INT16:
        return TB_SHADER_COMPONENT_INT16;
    case TB_SHADER_SCALAR_UINT16:
        return TB_SHADER_COMPONENT_UINT16;
    case TB_SHADER_SCALAR_INT64:
        return TB_SHADER_COMPONENT_INT64;
    case TB_SHADER_SCALAR_UINT64:
        return TB_SHADER_COMPONENT_UINT64;
    case TB_SHADER_SCALAR_MIN16INT:
        return TB_SHADER_COMPONENT_MIN16_INT;
    case TB_SHADER_SCALAR_MIN16UINT:
        return TB_SHADER_COMPONENT_MIN16_UINT;
    case TB_SHADER_SCALAR_MIN16FLOAT:
        return TB_SHADER_COMPONENT_MIN16_FLOAT;
    case TB_SHADER_SCALAR_DOUBLE:
        return TB_SHADER_COMPONENT_FLOAT64;
    case TB_SHADER_SCALAR_HALF: /* A float, as DXC takes it without 16-bit types. */
    case TB_SHADER_SCALAR_FLOAT:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
        return TB_SHADER_COMPONENT_FLOAT32;
    }
    return TB_SHADER_COMPONENT_FLOAT32;
}

static bool is_float_component(tb_shader_component component)
{
    return component == TB_SHADER_COMPONENT_FLOAT32 || component == TB_SHADER_COMPONENT_FLOAT16 ||
           component == TB_SHADER_COMPONENT_FLOAT64 || component == TB_SHADER_COMPONENT_MIN16_FLOAT;
}

/* How a pixel shader's input is interpolated: as its modifiers say, or, by
 * default, a float's linearly, SV_Position's without perspective, and any
 * other's not at all. */
static tb_shader_interpolation interpolation_of(const char *semantic, uint64_t modifiers,
                                                tb_shader_component component)
{
    if ((modifiers & TB_SHADER_MODIFIER_NOINTERPOLATION) != 0 || !is_float_component(component))
    {
        return TB_SHADER_INTERPOLATION_CONSTANT;
    }
    const bool no_perspective = (modifiers & TB_SHADER_MODIFIER_NOPERSPECTIVE) != 0 ||
                                tb_str_equal(semantic, "SV_Position");
    if ((modifiers & TB_SHADER_MODIFIER_SAMPLE) != 0)
    {
        return no_perspective ? TB_SHADER_INTERPOLATION_NOPERSPECTIVE_SAMPLE
                              : TB_SHADER_INTERPOLATION_SAMPLE;
    }
    if ((modifiers & TB_SHADER_MODIFIER_CENTROID) != 0)
    {
        return no_perspective ? TB_SHADER_INTERPOLATION_NOPERSPECTIVE_CENTROID
                              : TB_SHADER_INTERPOLATION_CENTROID;
    }
    return no_perspective ? TB_SHADER_INTERPOLATION_NOPERSPECTIVE : TB_SHADER_INTERPOLATION_LINEAR;
}

/* `TEXCOORD3`: its name, `TEXCOORD`, and its index, 3. */
static const char *split_semantic(reflector *state, const char *semantic, unsigned *index)
{
    size_t length = tb_str_length(semantic);
    while (length > 0 && isdigit((unsigned char)semantic[length - 1]))
    {
        length--;
    }
    *index = (unsigned)strtoul(semantic + length, NULL, 10);
    return tb_fmt_string(&state->allocator, "%.*s", (int)length, semantic);
}

/* The elements a value of `type` passes under `semantic`: one for a scalar
 * or a vector, one for each row of a matrix and each element of an array,
 * one for each member of a struct, under the member's own semantic. */
static void add_elements(reflector *state, tb_array *elements, uint32_t type, const char *semantic,
                         uint64_t modifiers, const char *owner)
{
    const tb_shader_form_type *const found = type_at(state, type);
    if (found->kind == TB_SHADER_FORM_STRUCT)
    {
        for (uint32_t at = 0; at < found->member_count; at++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(state->form, found->first_member + at);
            add_elements(state, elements, member->type, member->semantic,
                         modifiers | member->modifiers, member->name);
        }
        return;
    }
    if (semantic == NULL)
    {
        fail(state, "'%s' is passed between stages without a semantic", owner);
        return;
    }
    unsigned first = 0;
    const char *const name = split_semantic(state, semantic, &first);
    uint32_t rows = 1;
    uint32_t element = type;
    if (found->kind == TB_SHADER_FORM_ARRAY)
    {
        rows = found->length;
        element = found->element;
    }
    else if (found->kind == TB_SHADER_FORM_MATRIX)
    {
        rows = found->rows;
    }
    const tb_shader_form_type *const row = type_at(state, element);
    if (row->kind != TB_SHADER_FORM_SCALAR && row->kind != TB_SHADER_FORM_VECTOR &&
        row->kind != TB_SHADER_FORM_MATRIX)
    {
        fail(state, "'%s' is not a value a stage can pass", owner);
        return;
    }
    const tb_shader_component component = component_of(row->scalar);
    const bool interpolates = state->stage == TB_SHADER_STAGE_PIXEL && elements == state->inputs;
    for (uint32_t index = 0; index < rows; index++)
    {
        tb_shader_signature_element entry = {0};
        entry.semantic = name;
        entry.semantic_index = first + (unsigned)index;
        entry.component = component;
        entry.columns = row->kind == TB_SHADER_FORM_SCALAR ? 1u : (unsigned)row->columns;
        entry.interpolation = interpolates ? interpolation_of(name, modifiers, component)
                                           : TB_SHADER_INTERPOLATION_NONE;
        (void)tb_array_push(elements, &entry);
    }
}

static bool has_signatures(tb_shader_stage stage)
{
    return stage == TB_SHADER_STAGE_VERTEX || stage == TB_SHADER_STAGE_PIXEL ||
           stage == TB_SHADER_STAGE_GEOMETRY || stage == TB_SHADER_STAGE_HULL ||
           stage == TB_SHADER_STAGE_DOMAIN || stage == TB_SHADER_STAGE_MESH;
}

/* A parameter's elements: a geometry shader's primitive and a patch pass
 * their element's, a stream gives its element's, a mesh shader gives its
 * vertices'. */
static void add_parameter(reflector *state, const tb_shader_form_parameter *parameter)
{
    const tb_shader_form_type *const type = type_at(state, parameter->type);
    tb_array *const into =
        parameter->direction == TB_SHADER_FORM_OUT ? state->outputs : state->inputs;
    if (type->kind == TB_SHADER_FORM_RESOURCE)
    {
        const bool is_stream = type->object == TB_SHADER_OBJECT_POINT_STREAM ||
                               type->object == TB_SHADER_OBJECT_LINE_STREAM ||
                               type->object == TB_SHADER_OBJECT_TRIANGLE_STREAM;
        if (type->element != TB_SHADER_FORM_NONE)
        {
            add_elements(state, is_stream ? state->outputs : state->inputs, type->element, NULL, 0,
                         parameter->name);
        }
        return;
    }
    const uint64_t primitive = TB_SHADER_MODIFIER_POINT | TB_SHADER_MODIFIER_LINE |
                               TB_SHADER_MODIFIER_TRIANGLE | TB_SHADER_MODIFIER_LINEADJ |
                               TB_SHADER_MODIFIER_TRIANGLEADJ | TB_SHADER_MODIFIER_VERTICES;
    if ((parameter->modifiers & primitive) != 0 && type->kind == TB_SHADER_FORM_ARRAY)
    {
        add_elements(state, into, type->element, parameter->semantic, parameter->modifiers,
                     parameter->name);
        return;
    }
    const uint64_t not_passed =
        TB_SHADER_MODIFIER_INDICES | TB_SHADER_MODIFIER_PRIMITIVES | TB_SHADER_MODIFIER_PAYLOAD;
    if ((parameter->modifiers & not_passed) != 0)
    {
        return;
    }
    add_elements(state, into, parameter->type, parameter->semantic, parameter->modifiers,
                 parameter->name);
    if (parameter->direction == TB_SHADER_FORM_INOUT)
    {
        add_elements(state, state->outputs, parameter->type, parameter->semantic,
                     parameter->modifiers, parameter->name);
    }
}

static void reflect_signatures(reflector *state, const tb_shader_form_function *entry)
{
    if (!has_signatures(state->stage))
    {
        return;
    }
    if (type_at(state, entry->result)->kind != TB_SHADER_FORM_VOID)
    {
        add_elements(state, state->outputs, entry->result, entry->result_semantic, 0, entry->name);
    }
    const size_t count = tb_array_length(entry->parameters);
    for (size_t index = 0; index < count; index++)
    {
        add_parameter(state, TB_ARRAY_AT(entry->parameters, tb_shader_form_parameter, index));
    }
}

/* -------------------------------------------------------------------------
 * Bindings.
 * ------------------------------------------------------------------------- */

static tb_shader_binding_shape shape_of(tb_shader_object object)
{
    static const struct
    {
        tb_shader_object object;
        tb_shader_binding_shape shape;
    } shapes[] = {
        {TB_SHADER_OBJECT_BUFFER, TB_SHADER_SHAPE_BUFFER},
        {TB_SHADER_OBJECT_RW_BUFFER, TB_SHADER_SHAPE_BUFFER},
        {TB_SHADER_OBJECT_RO_BUFFER, TB_SHADER_SHAPE_BUFFER},
        {TB_SHADER_OBJECT_STRUCTURED_BUFFER, TB_SHADER_SHAPE_STRUCTURED_BUFFER},
        {TB_SHADER_OBJECT_RW_STRUCTURED_BUFFER, TB_SHADER_SHAPE_STRUCTURED_BUFFER},
        {TB_SHADER_OBJECT_RO_STRUCTURED_BUFFER, TB_SHADER_SHAPE_STRUCTURED_BUFFER},
        {TB_SHADER_OBJECT_APPEND_STRUCTURED_BUFFER, TB_SHADER_SHAPE_STRUCTURED_BUFFER},
        {TB_SHADER_OBJECT_CONSUME_STRUCTURED_BUFFER, TB_SHADER_SHAPE_STRUCTURED_BUFFER},
        {TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER, TB_SHADER_SHAPE_BYTE_ADDRESS_BUFFER},
        {TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER, TB_SHADER_SHAPE_BYTE_ADDRESS_BUFFER},
        {TB_SHADER_OBJECT_RO_BYTE_ADDRESS_BUFFER, TB_SHADER_SHAPE_BYTE_ADDRESS_BUFFER},
        {TB_SHADER_OBJECT_TEXTURE1D, TB_SHADER_SHAPE_TEXTURE1D},
        {TB_SHADER_OBJECT_RW_TEXTURE1D, TB_SHADER_SHAPE_TEXTURE1D},
        {TB_SHADER_OBJECT_RO_TEXTURE1D, TB_SHADER_SHAPE_TEXTURE1D},
        {TB_SHADER_OBJECT_TEXTURE1D_ARRAY, TB_SHADER_SHAPE_TEXTURE1D_ARRAY},
        {TB_SHADER_OBJECT_RW_TEXTURE1D_ARRAY, TB_SHADER_SHAPE_TEXTURE1D_ARRAY},
        {TB_SHADER_OBJECT_RO_TEXTURE1D_ARRAY, TB_SHADER_SHAPE_TEXTURE1D_ARRAY},
        {TB_SHADER_OBJECT_TEXTURE2D, TB_SHADER_SHAPE_TEXTURE2D},
        {TB_SHADER_OBJECT_RW_TEXTURE2D, TB_SHADER_SHAPE_TEXTURE2D},
        {TB_SHADER_OBJECT_RO_TEXTURE2D, TB_SHADER_SHAPE_TEXTURE2D},
        {TB_SHADER_OBJECT_TEXTURE2D_ARRAY, TB_SHADER_SHAPE_TEXTURE2D_ARRAY},
        {TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY, TB_SHADER_SHAPE_TEXTURE2D_ARRAY},
        {TB_SHADER_OBJECT_RO_TEXTURE2D_ARRAY, TB_SHADER_SHAPE_TEXTURE2D_ARRAY},
        {TB_SHADER_OBJECT_TEXTURE2DMS, TB_SHADER_SHAPE_TEXTURE2DMS},
        {TB_SHADER_OBJECT_RW_TEXTURE2DMS, TB_SHADER_SHAPE_TEXTURE2DMS},
        {TB_SHADER_OBJECT_TEXTURE2DMS_ARRAY, TB_SHADER_SHAPE_TEXTURE2DMS_ARRAY},
        {TB_SHADER_OBJECT_RW_TEXTURE2DMS_ARRAY, TB_SHADER_SHAPE_TEXTURE2DMS_ARRAY},
        {TB_SHADER_OBJECT_TEXTURE3D, TB_SHADER_SHAPE_TEXTURE3D},
        {TB_SHADER_OBJECT_RW_TEXTURE3D, TB_SHADER_SHAPE_TEXTURE3D},
        {TB_SHADER_OBJECT_RO_TEXTURE3D, TB_SHADER_SHAPE_TEXTURE3D},
        {TB_SHADER_OBJECT_TEXTURE_CUBE, TB_SHADER_SHAPE_TEXTURE_CUBE},
        {TB_SHADER_OBJECT_TEXTURE_CUBE_ARRAY, TB_SHADER_SHAPE_TEXTURE_CUBE_ARRAY},
        {TB_SHADER_OBJECT_ACCELERATION_STRUCTURE, TB_SHADER_SHAPE_ACCELERATION_STRUCTURE},
        {TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D, TB_SHADER_SHAPE_FEEDBACK_TEXTURE2D},
        {TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D_ARRAY, TB_SHADER_SHAPE_FEEDBACK_TEXTURE2D_ARRAY},
    };
    for (size_t index = 0; index < sizeof(shapes) / sizeof(shapes[0]); index++)
    {
        if (shapes[index].object == object)
        {
            return shapes[index].shape;
        }
    }
    return TB_SHADER_SHAPE_NONE;
}

static tb_shader_binding_kind kind_of(const tb_shader_form_global *global)
{
    switch (global->binding.register_class)
    {
    case 'b':
        return TB_SHADER_BINDING_CONSTANT_BUFFER;
    case 's':
        return TB_SHADER_BINDING_SAMPLER;
    case 'u':
        return TB_SHADER_BINDING_READ_WRITE;
    default:
        return global->space == TB_SHADER_FORM_UNIFORM ? TB_SHADER_BINDING_TEXTURE_BUFFER
                                                       : TB_SHADER_BINDING_READ;
    }
}

/* A global's binding, its register not assigned yet. */
static tb_shader_binding binding_of(const reflector *state, const tb_shader_form_global *global)
{
    tb_shader_binding binding = {0};
    binding.name = global->name;
    binding.kind = kind_of(global);
    binding.count = 1;
    const tb_shader_form_type *type = type_at(state, global->type);
    if (global->space == TB_SHADER_FORM_HANDLE && type->kind == TB_SHADER_FORM_ARRAY)
    {
        binding.count = type->length;
        type = type_at(state, type->element);
    }
    if (type->kind != TB_SHADER_FORM_RESOURCE)
    {
        return binding;
    }
    binding.shape = shape_of(type->object);
    if (type->element != TB_SHADER_FORM_NONE)
    {
        const tb_shader_form_type *const element = type_at(state, type->element);
        if (element->kind == TB_SHADER_FORM_SCALAR || element->kind == TB_SHADER_FORM_VECTOR)
        {
            binding.element = component_of(element->scalar);
            binding.element_columns =
                element->kind == TB_SHADER_FORM_SCALAR ? 1u : element->columns;
        }
    }
    return binding;
}

/* Registers taken in space 0 of a class, for assigning the others. */
typedef struct taken
{
    tb_array *ranges; /* uint32_t pairs: first, end. */
} taken;

static bool overlaps(const taken *registers, uint32_t first, uint32_t count)
{
    const size_t pairs = tb_array_length(registers->ranges) / 2;
    const uint32_t *const ranges = TB_ARRAY_DATA(registers->ranges, uint32_t);
    for (size_t index = 0; index < pairs; index++)
    {
        if (first < ranges[index * 2 + 1] && ranges[index * 2] < first + count)
        {
            return true;
        }
    }
    return false;
}

static void take(taken *registers, uint32_t first, uint32_t count)
{
    const uint32_t range[2] = {first, first + count};
    tb_array_append(registers->ranges, range, 2);
}

static bool is_globals(const tb_shader_form_global *global)
{
    return tb_str_equal(global->name, "$Globals");
}

/* Assign the bindings of one register class: explicit ones keep their
 * registers; the rest, `$Globals` first, then in the order of the source,
 * take the lowest registers of space 0 still free. */
static void bind_class(reflector *state, char register_class)
{
    taken registers = {TB_ARRAY_CREATE(&state->allocator, uint32_t)};
    const size_t count = tb_array_length(state->form->globals);
    const tb_shader_form_global *const globals =
        TB_ARRAY_DATA(state->form->globals, tb_shader_form_global);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_global *const global = &globals[index];
        const tb_shader_binding binding = binding_of(state, global);
        if (global->binding.register_class == register_class && global->binding.is_explicit &&
            global->binding.space == 0)
        {
            take(&registers, global->binding.index,
                 binding.count == 0 ? UINT32_MAX - global->binding.index : binding.count);
        }
    }
    /* `$Globals` first, then every other in order: two passes. */
    for (int pass = 0; pass < 2; pass++)
    {
        for (size_t index = 0; index < count; index++)
        {
            const tb_shader_form_global *const global = &globals[index];
            const bool in_pass = pass == 0 ? is_globals(global) : !is_globals(global);
            if (global->binding.register_class != register_class || !in_pass)
            {
                continue;
            }
            tb_shader_binding binding = binding_of(state, global);
            if (global->binding.is_explicit)
            {
                binding.register_index = global->binding.index;
                binding.space = global->binding.space;
            }
            else if (binding.count == 0)
            {
                fail(state, "'%s', an array of resources of no size, needs a register",
                     global->name);
                continue;
            }
            else
            {
                uint32_t first = 0;
                while (overlaps(&registers, first, binding.count))
                {
                    first++;
                }
                take(&registers, first, binding.count);
                binding.register_index = first;
            }
            (void)tb_array_push(state->bindings, &binding);
        }
    }
}

static void reflect_bindings(reflector *state)
{
    static const char classes[] = {'b', 's', 't', 'u'};
    for (size_t index = 0; index < sizeof(classes); index++)
    {
        bind_class(state, classes[index]);
    }
}

/* -------------------------------------------------------------------------
 * The entry point.
 * ------------------------------------------------------------------------- */

static void reflect_thread_group(const tb_shader_form_function *entry,
                                 tb_shader_reflection *reflection)
{
    const size_t count = tb_array_length(entry->attributes);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_attribute *const attribute =
            TB_ARRAY_AT(entry->attributes, tb_shader_form_attribute, index);
        if (!tb_str_equal(attribute->name, "numthreads") || attribute->argument_count != 3)
        {
            continue;
        }
        for (uint32_t axis = 0; axis < 3; axis++)
        {
            reflection->thread_group[axis] = (unsigned)attribute->arguments[axis].integer;
        }
    }
}

const char *tb_shader_reflect(const tb_shader_form *form, tb_arena *arena,
                              tb_shader_reflection *reflection)
{
    const tb_shader_reflection empty = {0};
    *reflection = empty;
    if (tb_array_length(form->entry_points) != 1)
    {
        return "a form of one entry point is reflected";
    }
    const tb_shader_form_entry_point *const entry =
        TB_ARRAY_AT(form->entry_points, tb_shader_form_entry_point, 0);
    reflector state = {0};
    state.form = form;
    state.arena = arena;
    state.allocator = tb_arena_allocator(arena);
    state.stage = (tb_shader_stage)entry->stage;
    state.inputs = TB_ARRAY_CREATE(&state.allocator, tb_shader_signature_element);
    state.outputs = TB_ARRAY_CREATE(&state.allocator, tb_shader_signature_element);
    state.bindings = TB_ARRAY_CREATE(&state.allocator, tb_shader_binding);
    const tb_shader_form_function *const function =
        tb_shader_form_function_at(form, entry->function);
    reflection->stage = state.stage;
    reflect_thread_group(function, reflection);
    reflect_signatures(&state, function);
    reflect_bindings(&state);
    reflection->inputs = tb_array_data(state.inputs);
    reflection->input_count = tb_array_length(state.inputs);
    reflection->outputs = tb_array_data(state.outputs);
    reflection->output_count = tb_array_length(state.outputs);
    reflection->bindings = tb_array_data(state.bindings);
    reflection->binding_count = tb_array_length(state.bindings);
    return state.error;
}
