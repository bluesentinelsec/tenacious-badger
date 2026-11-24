/* Reflection, read from the form of one entry point: its stage and thread
 * group, the values it takes and gives by their semantics, and the buffers,
 * textures and samplers it binds, with registers assigned as DXC assigns
 * them. */
#include "shader_reflect.h"
#include "shader_intrinsics.h"
#include "shader_methods.h"

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
    /* The blocks of rows of each signature, packed into registers once all
     * its elements are known. */
    tb_array *input_blocks;  /* signature_block */
    tb_array *output_blocks; /* signature_block */
    /* A hull shader's patch constants, or a domain shader's. */
    tb_array *patch_constants; /* tb_shader_signature_element */
    tb_array *patch_blocks;    /* signature_block */
    const char *domain;        /* `[domain("tri")]`: "tri", "quad", "isoline"; NULL for none. */
    const char *error;         /* The first reason the entry point cannot run. */
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

/* How an element is interpolated: as its modifiers say, or, by
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

/* The signatures of an entry point. */
typedef enum signature
{
    SIGNATURE_INPUT,
    SIGNATURE_OUTPUT,
    SIGNATURE_PATCH /* A hull shader's patch constants given, or a domain shader's taken. */
} signature;

/* Where a signature is in the pipeline, which decides how DXC treats each
 * system value in it. */
typedef enum signature_point
{
    POINT_VERTEX_INPUT, /* From the input assembler: a register each, unpacked. */
    POINT_PASSED,       /* Between stages: a vertex, domain, hull or geometry output. */
    POINT_PIXEL_INPUT,
    POINT_PIXEL_OUTPUT,
    POINT_GEOMETRY_INPUT, /* A geometry shader's vertex. */
    POINT_HULL_INPUT,     /* A hull shader's control point. */
    POINT_DOMAIN_INPUT,   /* A domain shader's control point. */
    POINT_PATCH_CONSTANT
} signature_point;

/* How a signature takes an element, as DXC's table of system values says.
 * The first three are packed, and in a register they come in this order:
 * values of the shader's own, then system values, then values the
 * rasterizer generates. */
typedef enum semantic_kind
{
    SEMANTIC_ARBITRARY, /* A semantic of the shader's own: `TEXCOORD`. */
    SEMANTIC_SYSTEM,    /* A system value: `SV_Position`. */
    SEMANTIC_GENERATED, /* Generated for a pixel shader: `SV_PrimitiveID`, `SV_IsFrontFace`. */
    SEMANTIC_CLIP_CULL, /* Packed in rows of clip and cull distances only. */
    /* SV_TessFactor, SV_InsideTessFactor: where the tessellator reads them. */
    SEMANTIC_TESS_FACTOR,
    SEMANTIC_TARGET,   /* SV_Target: the register of its index. */
    SEMANTIC_UNPACKED, /* Listed, in no register: SV_Depth, SV_SampleIndex. */
    SEMANTIC_ABSENT    /* Not in the signature: SV_Coverage taken, SV_GSInstanceID. */
} semantic_kind;

/* The rows an element takes together: an array's or a matrix's, packed as
 * one block at the same components of consecutive registers. */
typedef struct signature_block
{
    size_t first; /* Its first element. */
    uint32_t rows;
    unsigned columns;
    semantic_kind kind;
    tb_shader_interpolation mode; /* Rows of one mode only share a register. */
} signature_block;

/* DXC compares semantics without case. */
static bool same_semantic(const char *left, const char *right)
{
    for (; *left != '\0' && *right != '\0'; left++, right++)
    {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
        {
            return false;
        }
    }
    return *left == *right;
}

static signature_point point_of(tb_shader_stage stage, signature which)
{
    const bool is_input = which == SIGNATURE_INPUT;
    if (which == SIGNATURE_PATCH)
    {
        return POINT_PATCH_CONSTANT;
    }
    if (stage == TB_SHADER_STAGE_VERTEX && is_input)
    {
        return POINT_VERTEX_INPUT;
    }
    if (stage == TB_SHADER_STAGE_PIXEL)
    {
        return is_input ? POINT_PIXEL_INPUT : POINT_PIXEL_OUTPUT;
    }
    if (!is_input)
    {
        return POINT_PASSED;
    }
    if (stage == TB_SHADER_STAGE_GEOMETRY)
    {
        return POINT_GEOMETRY_INPUT;
    }
    return stage == TB_SHADER_STAGE_HULL ? POINT_HULL_INPUT : POINT_DOMAIN_INPUT;
}

/* How a signature takes a semantic: the system values it treats apart,
 * then any other system value, then the shader's own semantics. */
static semantic_kind kind_of_semantic(signature_point point, const char *semantic)
{
    static const struct
    {
        signature_point point;
        const char *semantic;
        semantic_kind kind;
    } kinds[] = {
        {POINT_PIXEL_INPUT, "SV_PrimitiveID", SEMANTIC_GENERATED},
        {POINT_PIXEL_INPUT, "SV_IsFrontFace", SEMANTIC_GENERATED},
        {POINT_PIXEL_INPUT, "SV_SampleIndex", SEMANTIC_UNPACKED},
        {POINT_PIXEL_INPUT, "SV_Barycentrics", SEMANTIC_UNPACKED},
        {POINT_PIXEL_INPUT, "SV_Coverage", SEMANTIC_ABSENT},
        {POINT_PIXEL_INPUT, "SV_InnerCoverage", SEMANTIC_ABSENT},
        {POINT_PIXEL_INPUT, "SV_CullPrimitive", SEMANTIC_ABSENT},
        {POINT_PIXEL_OUTPUT, "SV_Target", SEMANTIC_TARGET},
        {POINT_PIXEL_OUTPUT, "SV_Depth", SEMANTIC_UNPACKED},
        {POINT_PIXEL_OUTPUT, "SV_DepthGreaterEqual", SEMANTIC_UNPACKED},
        {POINT_PIXEL_OUTPUT, "SV_DepthLessEqual", SEMANTIC_UNPACKED},
        {POINT_PIXEL_OUTPUT, "SV_Coverage", SEMANTIC_UNPACKED},
        {POINT_PIXEL_OUTPUT, "SV_StencilRef", SEMANTIC_UNPACKED},
        {POINT_GEOMETRY_INPUT, "SV_PrimitiveID", SEMANTIC_UNPACKED},
        {POINT_GEOMETRY_INPUT, "SV_GSInstanceID", SEMANTIC_ABSENT},
        {POINT_HULL_INPUT, "SV_PrimitiveID", SEMANTIC_ABSENT},
        {POINT_HULL_INPUT, "SV_OutputControlPointID", SEMANTIC_ABSENT},
        {POINT_DOMAIN_INPUT, "SV_PrimitiveID", SEMANTIC_ABSENT},
        {POINT_DOMAIN_INPUT, "SV_DomainLocation", SEMANTIC_ABSENT},
        {POINT_PATCH_CONSTANT, "SV_TessFactor", SEMANTIC_TESS_FACTOR},
        {POINT_PATCH_CONSTANT, "SV_InsideTessFactor", SEMANTIC_TESS_FACTOR},
        /* A domain shader's parameters that are not its control points. */
        {POINT_PATCH_CONSTANT, "SV_PrimitiveID", SEMANTIC_ABSENT},
        {POINT_PATCH_CONSTANT, "SV_DomainLocation", SEMANTIC_ABSENT},
    };
    if (same_semantic(semantic, "SV_ViewID") && point != POINT_PASSED)
    {
        return SEMANTIC_ABSENT;
    }
    if (point != POINT_VERTEX_INPUT && point != POINT_PIXEL_OUTPUT &&
        (same_semantic(semantic, "SV_ClipDistance") || same_semantic(semantic, "SV_CullDistance")))
    {
        return SEMANTIC_CLIP_CULL;
    }
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        if (kinds[index].point == point && same_semantic(kinds[index].semantic, semantic))
        {
            return kinds[index].kind;
        }
    }
    const char prefix[] = "SV_";
    for (size_t index = 0; index + 1 < sizeof(prefix); index++)
    {
        if (toupper((unsigned char)semantic[index]) != prefix[index])
        {
            return SEMANTIC_ARBITRARY;
        }
    }
    return SEMANTIC_SYSTEM;
}

/* The elements a value of `type` passes under `semantic`: one for a scalar
 * or a vector, one for each element of an array, one for each column of a
 * matrix, or each row when `row_major`, one for each member of a struct,
 * under the member's own semantic. */
static tb_array *elements_of(const reflector *state, signature which)
{
    if (which == SIGNATURE_PATCH)
    {
        return state->patch_constants;
    }
    return which == SIGNATURE_INPUT ? state->inputs : state->outputs;
}

static tb_array *blocks_of(const reflector *state, signature which)
{
    if (which == SIGNATURE_PATCH)
    {
        return state->patch_blocks;
    }
    return which == SIGNATURE_INPUT ? state->input_blocks : state->output_blocks;
}

static void add_elements(reflector *state, signature which, uint32_t type, const char *semantic,
                         uint64_t modifiers, const char *owner)
{
    const tb_shader_form_type *const found = type_at(state, type);
    if (found->kind == TB_SHADER_FORM_STRUCT)
    {
        for (uint32_t at = 0; at < found->member_count; at++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(state->form, found->first_member + at);
            add_elements(state, which, member->type, member->semantic,
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
    uint32_t count = 1;
    const tb_shader_form_type *row = found;
    if (found->kind == TB_SHADER_FORM_ARRAY)
    {
        count = found->length;
        row = type_at(state, found->element);
    }
    if (row->kind != TB_SHADER_FORM_SCALAR && row->kind != TB_SHADER_FORM_VECTOR &&
        row->kind != TB_SHADER_FORM_MATRIX)
    {
        fail(state, "'%s' is not a value a stage can pass", owner);
        return;
    }
    const signature_point point = point_of(state->stage, which);
    const semantic_kind kind = kind_of_semantic(point, name);
    if (kind == SEMANTIC_ABSENT)
    {
        return;
    }
    unsigned columns = row->kind == TB_SHADER_FORM_SCALAR ? 1u : (unsigned)row->columns;
    if (row->kind == TB_SHADER_FORM_MATRIX)
    {
        const bool by_rows = (modifiers & TB_SHADER_MODIFIER_ROW_MAJOR) != 0;
        count *= by_rows ? row->rows : row->columns;
        columns = by_rows ? (unsigned)row->columns : (unsigned)row->rows;
    }
    tb_array *const elements = elements_of(state, which);
    const tb_shader_component component = component_of(row->scalar);
    /* Patch constants are not interpolated, and share registers whatever
     * their component. */
    const tb_shader_interpolation mode = point == POINT_PATCH_CONSTANT
                                             ? TB_SHADER_INTERPOLATION_NONE
                                             : interpolation_of(name, modifiers, component);
    signature_block block = {0};
    block.first = tb_array_length(elements);
    block.rows = count;
    block.columns = columns;
    block.kind = kind;
    block.mode = mode;
    (void)tb_array_push(blocks_of(state, which), &block);
    for (uint32_t index = 0; index < count; index++)
    {
        tb_shader_signature_element entry = {0};
        entry.semantic = name;
        entry.semantic_index = first + (unsigned)index;
        entry.component = component;
        entry.columns = columns;
        entry.interpolation = point == POINT_PIXEL_INPUT ? mode : TB_SHADER_INTERPOLATION_NONE;
        entry.register_index = TB_SHADER_NO_REGISTER;
        (void)tb_array_push(elements, &entry);
    }
}

/* A signature holds at most 32 registers of 4 components. */
#define SIGNATURE_REGISTERS 32u
#define SIGNATURE_COLUMNS 4u

/* What a register of a signature holds so far. */
typedef struct signature_register
{
    bool is_used;
    /* The kind of the element in each component; SEMANTIC_ABSENT for none. */
    semantic_kind kinds[4];
    tb_shader_interpolation mode;
} signature_register;

/* Where an element must be in a register beside others. */
static unsigned rank_of(semantic_kind kind)
{
    if (kind == SEMANTIC_ARBITRARY)
    {
        return 0;
    }
    /* Clip and cull distances and tessellation factors rank with system values. */
    return kind == SEMANTIC_GENERATED ? 2u : 1u;
}

/* Whether a register takes a block's row at `column`: components free, one
 * interpolation mode, clip and cull distances apart from the rest, and the
 * elements in order of their kind's rank. */
static bool row_fits(const signature_register *taken, const signature_block *block, unsigned column)
{
    if (!taken->is_used)
    {
        return true;
    }
    if (taken->mode != block->mode)
    {
        return false;
    }
    const bool is_clip_cull = block->kind == SEMANTIC_CLIP_CULL;
    const unsigned rank = rank_of(block->kind);
    for (unsigned at = 0; at < SIGNATURE_COLUMNS; at++)
    {
        const semantic_kind kind = taken->kinds[at];
        if (kind == SEMANTIC_ABSENT)
        {
            continue;
        }
        const bool inside = at >= column && at < column + block->columns;
        if (inside || (kind == SEMANTIC_CLIP_CULL) != is_clip_cull ||
            (at < column && rank_of(kind) > rank) ||
            (at >= column + block->columns && rank_of(kind) < rank))
        {
            return false;
        }
    }
    return true;
}

static bool block_fits(const signature_register *registers, const signature_block *block,
                       uint32_t first, unsigned column)
{
    if (first + block->rows > SIGNATURE_REGISTERS)
    {
        return false;
    }
    for (uint32_t row = first; row < first + block->rows; row++)
    {
        if (!row_fits(&registers[row], block, column))
        {
            return false;
        }
    }
    return true;
}

static void take_block(signature_register *registers, const signature_block *block, uint32_t first,
                       unsigned column)
{
    for (uint32_t row = first; row < first + block->rows; row++)
    {
        signature_register *const taken = &registers[row];
        taken->is_used = true;
        taken->mode = block->mode;
        for (unsigned at = column; at < column + block->columns; at++)
        {
            taken->kinds[at] = block->kind;
        }
    }
}

static void place_block(tb_array *elements, const signature_block *block, uint32_t first,
                        unsigned column)
{
    for (uint32_t row = 0; row < block->rows; row++)
    {
        tb_shader_signature_element *const element =
            TB_ARRAY_AT(elements, tb_shader_signature_element, block->first + row);
        element->register_index = first + row;
        element->first_column = column;
    }
}

/* Each block in the first register and components it fits, in the order
 * declared, as DXC packs a signature between stages. */
static void pack_block(reflector *state, signature_register *registers, tb_array *elements,
                       const signature_block *block)
{
    for (uint32_t first = 0; first < SIGNATURE_REGISTERS; first++)
    {
        for (unsigned column = 0; column + block->columns <= SIGNATURE_COLUMNS; column++)
        {
            if (block_fits(registers, block, first, column))
            {
                take_block(registers, block, first, column);
                place_block(elements, block, first, column);
                return;
            }
        }
    }
    const tb_shader_signature_element *const element =
        TB_ARRAY_AT(elements, tb_shader_signature_element, block->first);
    fail(state, "'%s' does not fit in the 32 registers a signature has", element->semantic);
}

/* Where the tessellator reads a tessellation factor: the edges' in the last
 * component of the first registers, one for each; a triangle's inside factor
 * in the first component of the register after its 3 edges', and a quad's 2
 * in the last component of the registers after its 4 edges'. */
static void place_tess_factor(const reflector *state, signature_register *registers,
                              tb_array *elements, const signature_block *block)
{
    const tb_shader_signature_element *const element =
        TB_ARRAY_AT(elements, tb_shader_signature_element, block->first);
    uint32_t first = element->semantic_index;
    unsigned column = SIGNATURE_COLUMNS - 1u;
    if (same_semantic(element->semantic, "SV_InsideTessFactor"))
    {
        const bool is_triangle =
            state->domain != NULL ? tb_str_equal(state->domain, "tri") : block->rows == 1;
        first += is_triangle ? 3u : 4u;
        column = is_triangle ? 0u : SIGNATURE_COLUMNS - 1u;
    }
    if (first + block->rows > SIGNATURE_REGISTERS || column + block->columns > SIGNATURE_COLUMNS)
    {
        return;
    }
    take_block(registers, block, first, column);
    place_block(elements, block, first, column);
}

/* Registers for a signature's elements, as DXC assigns them: a vertex
 * shader's inputs a register each, in order; a pixel shader's targets the
 * register of their index; tessellation factors where the tessellator reads
 * them; and every other element packed around them. */
static void pack_signature(reflector *state, signature which)
{
    tb_array *const elements = elements_of(state, which);
    tb_array *const blocks = blocks_of(state, which);
    const signature_point point = point_of(state->stage, which);
    signature_register registers[SIGNATURE_REGISTERS] = {0};
    for (size_t row = 0; row < SIGNATURE_REGISTERS; row++)
    {
        for (unsigned at = 0; at < SIGNATURE_COLUMNS; at++)
        {
            registers[row].kinds[at] = SEMANTIC_ABSENT;
        }
    }
    uint32_t next = 0;
    const size_t count = tb_array_length(blocks);
    for (size_t index = 0; index < count; index++)
    {
        const signature_block *const block = TB_ARRAY_AT(blocks, signature_block, index);
        if (block->kind == SEMANTIC_TESS_FACTOR)
        {
            place_tess_factor(state, registers, elements, block);
        }
    }
    for (size_t index = 0; index < count; index++)
    {
        const signature_block *const block = TB_ARRAY_AT(blocks, signature_block, index);
        if (block->kind == SEMANTIC_UNPACKED || block->kind == SEMANTIC_TESS_FACTOR)
        {
            continue;
        }
        if (block->kind == SEMANTIC_TARGET)
        {
            const tb_shader_signature_element *const element =
                TB_ARRAY_AT(elements, tb_shader_signature_element, block->first);
            place_block(elements, block, element->semantic_index, 0);
        }
        else if (point == POINT_VERTEX_INPUT)
        {
            place_block(elements, block, next, 0);
            next += block->rows;
        }
        else
        {
            pack_block(state, registers, elements, block);
        }
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
    /* A domain shader takes its control points from its OutputPatch, and
     * its patch constants from every other parameter. */
    signature which = state->stage == TB_SHADER_STAGE_DOMAIN ? SIGNATURE_PATCH : SIGNATURE_INPUT;
    if (parameter->direction == TB_SHADER_FORM_OUT)
    {
        which = SIGNATURE_OUTPUT;
    }
    if (type->kind == TB_SHADER_FORM_RESOURCE)
    {
        const bool is_stream = type->object == TB_SHADER_OBJECT_POINT_STREAM ||
                               type->object == TB_SHADER_OBJECT_LINE_STREAM ||
                               type->object == TB_SHADER_OBJECT_TRIANGLE_STREAM;
        if (type->element != TB_SHADER_FORM_NONE)
        {
            add_elements(state, is_stream ? SIGNATURE_OUTPUT : SIGNATURE_INPUT, type->element, NULL,
                         0, parameter->name);
        }
        return;
    }
    const uint64_t primitive = TB_SHADER_MODIFIER_POINT | TB_SHADER_MODIFIER_LINE |
                               TB_SHADER_MODIFIER_TRIANGLE | TB_SHADER_MODIFIER_LINEADJ |
                               TB_SHADER_MODIFIER_TRIANGLEADJ | TB_SHADER_MODIFIER_VERTICES;
    if ((parameter->modifiers & primitive) != 0 && type->kind == TB_SHADER_FORM_ARRAY)
    {
        add_elements(state, which, type->element, parameter->semantic, parameter->modifiers,
                     parameter->name);
        return;
    }
    const uint64_t not_passed =
        TB_SHADER_MODIFIER_INDICES | TB_SHADER_MODIFIER_PRIMITIVES | TB_SHADER_MODIFIER_PAYLOAD;
    if ((parameter->modifiers & not_passed) != 0)
    {
        return;
    }
    add_elements(state, which, parameter->type, parameter->semantic, parameter->modifiers,
                 parameter->name);
    if (parameter->direction == TB_SHADER_FORM_INOUT)
    {
        add_elements(state, SIGNATURE_OUTPUT, parameter->type, parameter->semantic,
                     parameter->modifiers, parameter->name);
    }
}

/* The patch constants a hull shader's patch-constant function gives: its
 * result's and its `out` parameters'. */
static void add_patch_constants(reflector *state, uint32_t entry_function)
{
    const uint32_t found = tb_shader_form_patch_constant_function(state->form, entry_function);
    const tb_shader_form_function *const entry =
        tb_shader_form_function_at(state->form, entry_function);
    if (found == TB_SHADER_FORM_NONE)
    {
        fail(state, "the hull shader '%s' names no patch-constant function it has", entry->name);
        return;
    }
    const tb_shader_form_function *const function = tb_shader_form_function_at(state->form, found);
    if (type_at(state, function->result)->kind != TB_SHADER_FORM_VOID)
    {
        add_elements(state, SIGNATURE_PATCH, function->result, function->result_semantic, 0,
                     function->name);
    }
    const size_t count = tb_array_length(function->parameters);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_parameter *const parameter =
            TB_ARRAY_AT(function->parameters, tb_shader_form_parameter, index);
        if (parameter->direction != TB_SHADER_FORM_IN)
        {
            add_elements(state, SIGNATURE_PATCH, parameter->type, parameter->semantic,
                         parameter->modifiers, parameter->name);
        }
    }
}

/* `[domain("tri")]`: the domain a hull or domain shader tessellates. */
static const char *domain_of(const tb_shader_form_function *entry)
{
    const size_t count = tb_array_length(entry->attributes);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_attribute *const attribute =
            TB_ARRAY_AT(entry->attributes, tb_shader_form_attribute, index);
        if (tb_str_equal(attribute->name, "domain") && attribute->argument_count == 1 &&
            attribute->arguments[0].kind == TB_SHADER_FORM_ARGUMENT_STRING)
        {
            return attribute->arguments[0].text;
        }
    }
    return NULL;
}

static void reflect_signatures(reflector *state, uint32_t entry_function)
{
    const tb_shader_form_function *const entry =
        tb_shader_form_function_at(state->form, entry_function);
    if (!has_signatures(state->stage))
    {
        return;
    }
    state->domain = domain_of(entry);
    if (type_at(state, entry->result)->kind != TB_SHADER_FORM_VOID)
    {
        add_elements(state, SIGNATURE_OUTPUT, entry->result, entry->result_semantic, 0,
                     entry->name);
    }
    const size_t count = tb_array_length(entry->parameters);
    for (size_t index = 0; index < count; index++)
    {
        add_parameter(state, TB_ARRAY_AT(entry->parameters, tb_shader_form_parameter, index));
    }
    if (state->stage == TB_SHADER_STAGE_HULL)
    {
        add_patch_constants(state, entry_function);
    }
    pack_signature(state, SIGNATURE_INPUT);
    pack_signature(state, SIGNATURE_OUTPUT);
    pack_signature(state, SIGNATURE_PATCH);
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
 * Constant buffers' layouts, as HLSL packs them.
 * ------------------------------------------------------------------------- */

#define REGISTER_BYTES 16u

static unsigned round_up(unsigned value, unsigned multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

static unsigned scalar_bytes(tb_shader_scalar scalar)
{
    return scalar == TB_SHADER_SCALAR_DOUBLE || scalar == TB_SHADER_SCALAR_INT64 ||
                   scalar == TB_SHADER_SCALAR_UINT64
               ? 8u
               : 4u;
}

static bool is_min_precision(tb_shader_scalar scalar)
{
    return scalar == TB_SHADER_SCALAR_MIN16FLOAT || scalar == TB_SHADER_SCALAR_MIN16INT ||
           scalar == TB_SHADER_SCALAR_MIN16UINT;
}

static tb_shader_variable *variable_at(tb_array *variables, size_t index)
{
    return TB_ARRAY_AT(variables, tb_shader_variable, index);
}

static void place(reflector *state, tb_array *variables, size_t slot, uint32_t type,
                  uint64_t modifiers, unsigned *cursor);

/* A struct's members, each after the one before, from a register's start. */
static void place_struct(reflector *state, tb_array *variables, size_t slot,
                         const tb_shader_form_type *structure, unsigned *cursor)
{
    const size_t first = tb_array_length(variables);
    for (uint32_t at = 0; at < structure->member_count; at++)
    {
        const tb_shader_variable empty = {0};
        (void)tb_array_push(variables, &empty);
    }
    variable_at(variables, slot)->first_member = (unsigned)first;
    variable_at(variables, slot)->member_count = structure->member_count;
    for (uint32_t at = 0; at < structure->member_count; at++)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(state->form, structure->first_member + at);
        variable_at(variables, first + at)->name = member->name;
        place(state, variables, first + at, member->type, member->modifiers, cursor);
    }
}

/* Lay a value of `type` out at the first offset at or after `*cursor` its
 * rules allow, into the variable at `slot`, and move the cursor past it:
 * a scalar or a vector where it does not cross a register, a minimum
 * precision one in a register of its own; a matrix, a struct and each
 * element of an array at a register's start, the last element unpadded. */
static void place(reflector *state, tb_array *variables, size_t slot, uint32_t type,
                  uint64_t modifiers, unsigned *cursor)
{
    const tb_shader_form_type *const found = type_at(state, type);
    if (found->kind == TB_SHADER_FORM_ARRAY)
    {
        const unsigned start = round_up(*cursor, REGISTER_BYTES);
        unsigned end = start;
        place(state, variables, slot, found->element, modifiers, &end);
        const unsigned element = end - start;
        const unsigned stride = round_up(element, REGISTER_BYTES);
        tb_shader_variable *const variable = variable_at(variables, slot);
        variable->elements = found->length;
        variable->stride = stride;
        variable->size = found->length == 0 ? element : stride * (found->length - 1) + element;
        *cursor = start + variable->size;
        return;
    }
    if (found->kind == TB_SHADER_FORM_STRUCT)
    {
        const unsigned start = round_up(*cursor, REGISTER_BYTES);
        tb_shader_variable *const variable = variable_at(variables, slot);
        variable->variable_class = TB_SHADER_VARIABLE_STRUCT;
        variable->offset = start;
        variable->rows = 1;
        unsigned end = start;
        place_struct(state, variables, slot, found, &end);
        variable_at(variables, slot)->size = end - start;
        *cursor = end;
        return;
    }
    tb_shader_variable *const variable = variable_at(variables, slot);
    variable->component = component_of(found->scalar);
    variable->rows = found->kind == TB_SHADER_FORM_MATRIX ? found->rows : 1u;
    variable->columns = found->kind == TB_SHADER_FORM_SCALAR ? 1u : found->columns;
    const unsigned bytes = scalar_bytes(found->scalar);
    if (found->kind == TB_SHADER_FORM_MATRIX)
    {
        const bool by_rows = (modifiers & TB_SHADER_MODIFIER_ROW_MAJOR) != 0;
        const unsigned registers = by_rows ? found->rows : found->columns;
        const unsigned per_register = by_rows ? found->columns : found->rows;
        variable->variable_class =
            by_rows ? TB_SHADER_VARIABLE_MATRIX_ROWS : TB_SHADER_VARIABLE_MATRIX_COLUMNS;
        variable->offset = round_up(*cursor, REGISTER_BYTES);
        variable->size = REGISTER_BYTES * (registers - 1) + per_register * bytes;
        *cursor = variable->offset + variable->size;
        return;
    }
    variable->variable_class = found->kind == TB_SHADER_FORM_SCALAR ? TB_SHADER_VARIABLE_SCALAR
                                                                    : TB_SHADER_VARIABLE_VECTOR;
    variable->size = variable->columns * bytes;
    if (is_min_precision(found->scalar))
    {
        variable->offset = round_up(*cursor, REGISTER_BYTES);
        *cursor = variable->offset + REGISTER_BYTES;
        return;
    }
    unsigned offset = round_up(*cursor, bytes);
    if (offset % REGISTER_BYTES + variable->size > REGISTER_BYTES)
    {
        offset = round_up(offset, REGISTER_BYTES);
    }
    variable->offset = offset;
    *cursor = offset + variable->size;
}

/* A buffer's layout: its variables in order, each after the one before, or
 * where `packoffset` puts it. */
static void lay_out_buffer(reflector *state, const tb_shader_form_global *global,
                           tb_shader_constant_buffer *buffer)
{
    tb_array *const variables = TB_ARRAY_CREATE(&state->allocator, tb_shader_variable);
    buffer->name = global->name;
    const tb_shader_form_type *const structure = type_at(state, global->type);
    if (structure->kind != TB_SHADER_FORM_STRUCT)
    {
        return;
    }
    for (uint32_t at = 0; at < structure->member_count; at++)
    {
        const tb_shader_variable empty = {0};
        (void)tb_array_push(variables, &empty);
    }
    unsigned cursor = 0;
    unsigned size = 0;
    for (uint32_t at = 0; at < structure->member_count; at++)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(state->form, structure->first_member + at);
        variable_at(variables, at)->name = member->name;
        unsigned position = member->packoffset != TB_SHADER_FORM_NONE ? member->packoffset : cursor;
        place(state, variables, at, member->type, member->modifiers, &position);
        if (member->packoffset != TB_SHADER_FORM_NONE)
        {
            variable_at(variables, at)->offset = member->packoffset;
        }
        cursor = position > cursor ? position : cursor;
        size = position > size ? position : size;
    }
    buffer->size = size;
    buffer->variables = tb_array_data(variables);
    buffer->variable_count = tb_array_length(variables);
    buffer->top_count = structure->member_count;
}

/* The layout of each constant and texture buffer bound, in their bindings' order. */
static void reflect_buffers(reflector *state, tb_shader_reflection *reflection)
{
    tb_array *const buffers = TB_ARRAY_CREATE(&state->allocator, tb_shader_constant_buffer);
    const size_t binding_count = tb_array_length(state->bindings);
    const size_t global_count = tb_array_length(state->form->globals);
    for (size_t index = 0; index < binding_count; index++)
    {
        const tb_shader_binding *const binding =
            TB_ARRAY_AT(state->bindings, tb_shader_binding, index);
        if (binding->kind != TB_SHADER_BINDING_CONSTANT_BUFFER &&
            binding->kind != TB_SHADER_BINDING_TEXTURE_BUFFER)
        {
            continue;
        }
        for (size_t at = 0; at < global_count; at++)
        {
            const tb_shader_form_global *const global =
                TB_ARRAY_AT(state->form->globals, tb_shader_form_global, at);
            if (global->space == TB_SHADER_FORM_UNIFORM &&
                tb_str_equal(global->name, binding->name))
            {
                tb_shader_constant_buffer buffer = {0};
                lay_out_buffer(state, global, &buffer);
                (void)tb_array_push(buffers, &buffer);
                break;
            }
        }
    }
    reflection->buffers = tb_array_data(buffers);
    reflection->buffer_count = tb_array_length(buffers);
}

/* -------------------------------------------------------------------------
 * Features: what the entry point needs beyond the portable core.
 * ------------------------------------------------------------------------- */

/* The features a type needs: a double's, a 64-bit integer's, a RayQuery's,
 * a feedback texture's, an array of resources of no size's. */
static unsigned type_features(const reflector *state, uint32_t type)
{
    const tb_shader_form_type *const found = type_at(state, type);
    switch (found->kind)
    {
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_VECTOR:
    case TB_SHADER_FORM_MATRIX:
        if (found->scalar == TB_SHADER_SCALAR_DOUBLE)
        {
            return TB_SHADER_FEATURE_FLOAT64;
        }
        return found->scalar == TB_SHADER_SCALAR_INT64 || found->scalar == TB_SHADER_SCALAR_UINT64
                   ? TB_SHADER_FEATURE_INT64
                   : 0u;
    case TB_SHADER_FORM_ARRAY: {
        const bool unbounded =
            found->length == 0 && type_at(state, found->element)->kind == TB_SHADER_FORM_RESOURCE;
        return type_features(state, found->element) | (unbounded ? TB_SHADER_FEATURE_BINDLESS : 0u);
    }
    case TB_SHADER_FORM_STRUCT: {
        unsigned features = 0;
        for (uint32_t at = 0; at < found->member_count; at++)
        {
            features |= type_features(
                state, tb_shader_form_member_at(state->form, found->first_member + at)->type);
        }
        return features;
    }
    case TB_SHADER_FORM_RESOURCE:
        if (found->object == TB_SHADER_OBJECT_RAY_QUERY)
        {
            return TB_SHADER_FEATURE_RAY_QUERY;
        }
        if (found->object == TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D ||
            found->object == TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D_ARRAY)
        {
            return TB_SHADER_FEATURE_SAMPLER_FEEDBACK;
        }
        return found->element == TB_SHADER_FORM_NONE ? 0u : type_features(state, found->element);
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_STRING:
        break;
    }
    return 0;
}

static bool starts_with(const char *text, const char *prefix)
{
    return tb_str_starts_with(text, prefix);
}

/* The features an intrinsic or a method needs: a wave's or a quad's
 * operation, NonUniformResourceIndex, an atomic of 64 bits. */
static unsigned call_features(const reflector *state, const tb_shader_form_function *function,
                              uint32_t intrinsic, uint32_t method, uint32_t first_operand,
                              uint32_t operand_count)
{
    unsigned features = 0;
    const tb_shader_intrinsic_info *const info =
        tb_shader_intrinsic_info_of((tb_shader_intrinsic)intrinsic);
    if (info != NULL)
    {
        if (starts_with(info->name, "Wave") || starts_with(info->name, "Quad"))
        {
            features |= TB_SHADER_FEATURE_WAVE_OPERATIONS;
        }
        if (intrinsic == (uint32_t)TB_SHADER_INTRINSIC_NON_UNIFORM_RESOURCE_INDEX)
        {
            features |= TB_SHADER_FEATURE_BINDLESS;
        }
        if (starts_with(info->name, "Interlocked") && operand_count > 0)
        {
            const uint32_t target = tb_shader_form_operand_at(function, first_operand);
            const tb_shader_form_type *const type =
                type_at(state, tb_shader_form_expression_at(function, target)->type);
            if (scalar_bytes(type->scalar) == 8)
            {
                features |= TB_SHADER_FEATURE_ATOMIC64;
            }
        }
    }
    const tb_shader_method_info *const called = tb_shader_method_info_of((tb_shader_method)method);
    if (called != NULL && starts_with(called->name, "Interlocked") &&
        tb_str_ends_with(called->name, "64"))
    {
        features |= TB_SHADER_FEATURE_ATOMIC64;
    }
    return features;
}

static unsigned function_features(const reflector *state, const tb_shader_form_function *function)
{
    unsigned features = 0;
    const size_t expression_count = tb_shader_form_expression_count(function);
    for (size_t index = 0; index < expression_count; index++)
    {
        const tb_shader_form_expression *const expression =
            tb_shader_form_expression_at(function, (uint32_t)index);
        features |= type_features(state, expression->type);
        if (expression->kind == TB_SHADER_FORM_INTRINSIC)
        {
            features |= call_features(state, function, expression->intrinsic, expression->method,
                                      expression->first_operand, expression->operand_count);
        }
    }
    const size_t local_count = tb_array_length(function->locals);
    for (size_t index = 0; index < local_count; index++)
    {
        features |=
            type_features(state, TB_ARRAY_AT(function->locals, tb_shader_form_local, index)->type);
    }
    const size_t statement_count = tb_shader_form_statement_count(function);
    for (size_t index = 0; index < statement_count; index++)
    {
        const tb_shader_form_statement *const statement =
            tb_shader_form_statement_at(function, (uint32_t)index);
        if (statement->kind == TB_SHADER_FORM_CALL)
        {
            features |= call_features(state, function, statement->intrinsic, statement->method,
                                      statement->first_operand, statement->operand_count);
        }
    }
    return features;
}

static unsigned stage_features(tb_shader_stage stage)
{
    if (stage == TB_SHADER_STAGE_MESH || stage == TB_SHADER_STAGE_AMPLIFICATION)
    {
        return TB_SHADER_FEATURE_MESH_SHADER;
    }
    return stage >= TB_SHADER_STAGE_RAY_GENERATION ? TB_SHADER_FEATURE_RAY_TRACING : 0u;
}

static unsigned reflect_features(const reflector *state)
{
    unsigned features = stage_features(state->stage);
    const size_t global_count = tb_array_length(state->form->globals);
    for (size_t index = 0; index < global_count; index++)
    {
        features |= type_features(
            state, TB_ARRAY_AT(state->form->globals, tb_shader_form_global, index)->type);
    }
    const size_t function_count = tb_shader_form_function_count(state->form);
    for (size_t index = 0; index < function_count; index++)
    {
        features |=
            function_features(state, tb_shader_form_function_at(state->form, (uint32_t)index));
    }
    return features;
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
    state.input_blocks = TB_ARRAY_CREATE(&state.allocator, signature_block);
    state.output_blocks = TB_ARRAY_CREATE(&state.allocator, signature_block);
    state.patch_constants = TB_ARRAY_CREATE(&state.allocator, tb_shader_signature_element);
    state.patch_blocks = TB_ARRAY_CREATE(&state.allocator, signature_block);
    const tb_shader_form_function *const function =
        tb_shader_form_function_at(form, entry->function);
    reflection->stage = state.stage;
    reflect_thread_group(function, reflection);
    reflect_signatures(&state, entry->function);
    reflect_bindings(&state);
    reflect_buffers(&state, reflection);
    reflection->features = reflect_features(&state);
    reflection->inputs = tb_array_data(state.inputs);
    reflection->input_count = tb_array_length(state.inputs);
    reflection->outputs = tb_array_data(state.outputs);
    reflection->output_count = tb_array_length(state.outputs);
    reflection->patch_constants = tb_array_data(state.patch_constants);
    reflection->patch_constant_count = tb_array_length(state.patch_constants);
    reflection->bindings = tb_array_data(state.bindings);
    reflection->binding_count = tb_array_length(state.bindings);
    return state.error;
}
