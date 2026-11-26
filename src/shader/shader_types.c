/* Types: interning, their names, the built-in objects and structs, and the
 * classes and ranks of scalars that conversions follow. */
#include "shader_types.h"

#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/str.h"

#include <string.h>

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

struct tb_shader_types
{
    tb_arena *arena;
    tb_allocator allocator;
    tb_table *interned; /* tb_shader_type to itself, by its key fields. */
    const tb_shader_type *error;
    const tb_shader_type *void_type;
    const tb_shader_type *string;
    const tb_shader_type *ray_desc;
    const tb_shader_type *triangle_attributes;
};

/* -------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

uint64_t tb_shader_name_hash(const void *key)
{
    const tb_shader_name *const name = key;
    return tb_hash_bytes(name->text, name->length);
}

bool tb_shader_name_equal(const void *left, const void *right)
{
    const tb_shader_name *const a = left;
    const tb_shader_name *const b = right;
    return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

tb_shader_name tb_shader_name_of(const char *text)
{
    const tb_shader_name name = {text, tb_str_length(text)};
    return name;
}

static const char *const scalar_names[TB_SHADER_SCALAR_COUNT] = {
    "bool",    "int",      "uint",     "int16_t",     "uint16_t",
    "int64_t", "uint64_t", "min16int", "min16uint",   "min16float",
    "half",    "float",    "double",   "literal int", "literal float",
};

/* -------------------------------------------------------------------------
 * Interning.
 * ------------------------------------------------------------------------- */

static uint64_t type_hash(const void *key)
{
    const tb_shader_type *const type = key;
    const uint64_t parts[] = {(uint64_t)type->type_class,
                              (uint64_t)type->scalar,
                              type->rows,
                              type->columns,
                              (uint64_t)(uintptr_t)type->element,
                              type->length,
                              type->second_length,
                              (uint64_t)type->object};
    return tb_hash_bytes(parts, sizeof(parts));
}

static bool type_equal(const void *left, const void *right)
{
    const tb_shader_type *const a = left;
    const tb_shader_type *const b = right;
    return a->type_class == b->type_class && a->scalar == b->scalar && a->rows == b->rows &&
           a->columns == b->columns && a->element == b->element && a->length == b->length &&
           a->second_length == b->second_length && a->object == b->object;
}

/* The interned type equal to `probe`, named by `name` when it is new. */
static const tb_shader_type *intern(tb_shader_types *types, const tb_shader_type *probe,
                                    const char *name)
{
    const tb_shader_type *const found = tb_table_get(types->interned, probe);
    if (found != NULL)
    {
        return found;
    }
    tb_shader_type *const type = TB_ARENA_NEW(types->arena, tb_shader_type);
    *type = *probe;
    type->name = name;
    (void)tb_table_put(types->interned, type, type);
    return type;
}

static tb_shader_type new_probe(tb_shader_type_class type_class)
{
    tb_shader_type probe = {0};
    probe.type_class = type_class;
    probe.rows = 1;
    probe.columns = 1;
    return probe;
}

/* -------------------------------------------------------------------------
 * Numeric types.
 * ------------------------------------------------------------------------- */

const tb_shader_type *tb_shader_scalar_type(tb_shader_types *types, tb_shader_scalar scalar)
{
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_SCALAR);
    probe.scalar = scalar;
    return intern(types, &probe, scalar_names[scalar]);
}

const tb_shader_type *tb_shader_vector_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            uint32_t columns)
{
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_VECTOR);
    probe.scalar = scalar;
    probe.columns = columns;
    if (tb_table_get(types->interned, &probe) != NULL)
    {
        return tb_table_get(types->interned, &probe);
    }
    return intern(
        types, &probe,
        tb_fmt_string(&types->allocator, "%s%u", scalar_names[scalar], (unsigned)columns));
}

const tb_shader_type *tb_shader_matrix_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            uint32_t rows, uint32_t columns)
{
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_MATRIX);
    probe.scalar = scalar;
    probe.rows = rows;
    probe.columns = columns;
    if (tb_table_get(types->interned, &probe) != NULL)
    {
        return tb_table_get(types->interned, &probe);
    }
    return intern(types, &probe,
                  tb_fmt_string(&types->allocator, "%s%ux%u", scalar_names[scalar], (unsigned)rows,
                                (unsigned)columns));
}

const tb_shader_type *tb_shader_shaped_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            const tb_shader_type *shape)
{
    if (shape->type_class == TB_SHADER_CLASS_VECTOR)
    {
        return tb_shader_vector_type(types, scalar, shape->columns);
    }
    if (shape->type_class == TB_SHADER_CLASS_MATRIX)
    {
        return tb_shader_matrix_type(types, scalar, shape->rows, shape->columns);
    }
    return tb_shader_scalar_type(types, scalar);
}

typedef struct scalar_spelling
{
    const char *text;
    tb_shader_scalar scalar;
} scalar_spelling;

/* Longest first where one spelling begins another. */
static const scalar_spelling scalar_spellings[] = {
    {"min16float", TB_SHADER_SCALAR_MIN16FLOAT},
    {"min10float", TB_SHADER_SCALAR_MIN16FLOAT},
    {"min16uint", TB_SHADER_SCALAR_MIN16UINT},
    {"min16int", TB_SHADER_SCALAR_MIN16INT},
    {"min12int", TB_SHADER_SCALAR_MIN16INT},
    {"float16_t", TB_SHADER_SCALAR_HALF},
    {"float32_t", TB_SHADER_SCALAR_FLOAT},
    {"float64_t", TB_SHADER_SCALAR_DOUBLE},
    {"uint16_t", TB_SHADER_SCALAR_UINT16},
    {"uint32_t", TB_SHADER_SCALAR_UINT},
    {"uint64_t", TB_SHADER_SCALAR_UINT64},
    {"int16_t", TB_SHADER_SCALAR_INT16},
    {"int32_t", TB_SHADER_SCALAR_INT},
    {"int64_t", TB_SHADER_SCALAR_INT64},
    {"double", TB_SHADER_SCALAR_DOUBLE},
    {"dword", TB_SHADER_SCALAR_UINT},
    {"float", TB_SHADER_SCALAR_FLOAT},
    {"half", TB_SHADER_SCALAR_HALF},
    {"bool", TB_SHADER_SCALAR_BOOL},
    {"uint", TB_SHADER_SCALAR_UINT},
    {"int", TB_SHADER_SCALAR_INT},
};

static bool is_dimension(char byte)
{
    return byte >= '1' && byte <= '4';
}

const tb_shader_type *tb_shader_numeric_type(tb_shader_types *types, tb_shader_name name)
{
    for (size_t index = 0; index < COUNT_OF(scalar_spellings); index++)
    {
        const size_t length = tb_str_length(scalar_spellings[index].text);
        if (name.length < length || memcmp(name.text, scalar_spellings[index].text, length) != 0)
        {
            continue;
        }
        const tb_shader_scalar scalar = scalar_spellings[index].scalar;
        const char *const rest = name.text + length;
        const size_t rest_length = name.length - length;
        if (rest_length == 0)
        {
            return tb_shader_scalar_type(types, scalar);
        }
        if (rest_length == 1 && is_dimension(rest[0]))
        {
            return tb_shader_vector_type(types, scalar, (uint32_t)(rest[0] - '0'));
        }
        if (rest_length == 3 && is_dimension(rest[0]) && rest[1] == 'x' && is_dimension(rest[2]))
        {
            return tb_shader_matrix_type(types, scalar, (uint32_t)(rest[0] - '0'),
                                         (uint32_t)(rest[2] - '0'));
        }
        return NULL;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Arrays, objects, structs and enums.
 * ------------------------------------------------------------------------- */

const tb_shader_type *tb_shader_array_type(tb_shader_types *types, const tb_shader_type *element,
                                           uint32_t length)
{
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_ARRAY);
    probe.element = element;
    probe.length = length;
    if (tb_table_get(types->interned, &probe) != NULL)
    {
        return tb_table_get(types->interned, &probe);
    }
    /* `int[2][3]` is an array of 2 of `int[3]`: its own size goes first. */
    const char *const element_name = element->name;
    const size_t bracket = tb_str_find_char(element_name, 0, TB_STR_END, '[');
    const size_t prefix = bracket == TB_STR_NOT_FOUND ? tb_str_length(element_name) : bracket;
    char size[16] = {0};
    if (length > 0)
    {
        (void)tb_fmt_buffer(size, sizeof(size), "%u", (unsigned)length);
    }
    const char *const name = tb_fmt_string(&types->allocator, "%.*s[%s]%s", (int)prefix,
                                           element_name, size, element_name + prefix);
    return intern(types, &probe, name);
}

static const tb_shader_object_info objects[] = {
    {TB_SHADER_OBJECT_SAMPLER, "SamplerState", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER, "sampler", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER, "sampler1D", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER, "sampler2D", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER, "sampler3D", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER, "samplerCUBE", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_SAMPLER_COMPARISON, "SamplerComparisonState", TB_SHADER_TEMPLATE_NONE, false,
     0},
    {TB_SHADER_OBJECT_TEXTURE, "texture", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_BUFFER, "Buffer", TB_SHADER_TEMPLATE_ELEMENT, false, 1},
    {TB_SHADER_OBJECT_RW_BUFFER, "RWBuffer", TB_SHADER_TEMPLATE_ELEMENT, true, 1},
    {TB_SHADER_OBJECT_RO_BUFFER, "RasterizerOrderedBuffer", TB_SHADER_TEMPLATE_ELEMENT, true, 1},
    {TB_SHADER_OBJECT_STRUCTURED_BUFFER, "StructuredBuffer", TB_SHADER_TEMPLATE_STRUCTURE, false,
     1},
    {TB_SHADER_OBJECT_RW_STRUCTURED_BUFFER, "RWStructuredBuffer", TB_SHADER_TEMPLATE_STRUCTURE,
     true, 1},
    {TB_SHADER_OBJECT_RO_STRUCTURED_BUFFER, "RasterizerOrderedStructuredBuffer",
     TB_SHADER_TEMPLATE_STRUCTURE, true, 1},
    {TB_SHADER_OBJECT_APPEND_STRUCTURED_BUFFER, "AppendStructuredBuffer",
     TB_SHADER_TEMPLATE_STRUCTURE, true, 0},
    {TB_SHADER_OBJECT_CONSUME_STRUCTURED_BUFFER, "ConsumeStructuredBuffer",
     TB_SHADER_TEMPLATE_STRUCTURE, true, 0},
    {TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER, "ByteAddressBuffer", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER, "RWByteAddressBuffer", TB_SHADER_TEMPLATE_NONE, true,
     0},
    {TB_SHADER_OBJECT_RO_BYTE_ADDRESS_BUFFER, "RasterizerOrderedByteAddressBuffer",
     TB_SHADER_TEMPLATE_NONE, true, 0},
    {TB_SHADER_OBJECT_TEXTURE1D, "Texture1D", TB_SHADER_TEMPLATE_ELEMENT, false, 1},
    {TB_SHADER_OBJECT_TEXTURE1D_ARRAY, "Texture1DArray", TB_SHADER_TEMPLATE_ELEMENT, false, 2},
    {TB_SHADER_OBJECT_TEXTURE2D, "Texture2D", TB_SHADER_TEMPLATE_ELEMENT, false, 2},
    {TB_SHADER_OBJECT_TEXTURE2D_ARRAY, "Texture2DArray", TB_SHADER_TEMPLATE_ELEMENT, false, 3},
    {TB_SHADER_OBJECT_TEXTURE2DMS, "Texture2DMS", TB_SHADER_TEMPLATE_SAMPLES, false, 0},
    {TB_SHADER_OBJECT_TEXTURE2DMS_ARRAY, "Texture2DMSArray", TB_SHADER_TEMPLATE_SAMPLES, false, 0},
    {TB_SHADER_OBJECT_TEXTURE3D, "Texture3D", TB_SHADER_TEMPLATE_ELEMENT, false, 3},
    {TB_SHADER_OBJECT_TEXTURE_CUBE, "TextureCube", TB_SHADER_TEMPLATE_ELEMENT, false, 0},
    {TB_SHADER_OBJECT_TEXTURE_CUBE_ARRAY, "TextureCubeArray", TB_SHADER_TEMPLATE_ELEMENT, false, 0},
    {TB_SHADER_OBJECT_RW_TEXTURE1D, "RWTexture1D", TB_SHADER_TEMPLATE_ELEMENT, true, 1},
    {TB_SHADER_OBJECT_RW_TEXTURE1D_ARRAY, "RWTexture1DArray", TB_SHADER_TEMPLATE_ELEMENT, true, 2},
    {TB_SHADER_OBJECT_RW_TEXTURE2D, "RWTexture2D", TB_SHADER_TEMPLATE_ELEMENT, true, 2},
    {TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY, "RWTexture2DArray", TB_SHADER_TEMPLATE_ELEMENT, true, 3},
    {TB_SHADER_OBJECT_RW_TEXTURE2DMS, "RWTexture2DMS", TB_SHADER_TEMPLATE_SAMPLES, true, 0},
    {TB_SHADER_OBJECT_RW_TEXTURE2DMS_ARRAY, "RWTexture2DMSArray", TB_SHADER_TEMPLATE_SAMPLES, true,
     0},
    {TB_SHADER_OBJECT_RW_TEXTURE3D, "RWTexture3D", TB_SHADER_TEMPLATE_ELEMENT, true, 3},
    {TB_SHADER_OBJECT_RO_TEXTURE1D, "RasterizerOrderedTexture1D", TB_SHADER_TEMPLATE_ELEMENT, true,
     1},
    {TB_SHADER_OBJECT_RO_TEXTURE1D_ARRAY, "RasterizerOrderedTexture1DArray",
     TB_SHADER_TEMPLATE_ELEMENT, true, 2},
    {TB_SHADER_OBJECT_RO_TEXTURE2D, "RasterizerOrderedTexture2D", TB_SHADER_TEMPLATE_ELEMENT, true,
     2},
    {TB_SHADER_OBJECT_RO_TEXTURE2D_ARRAY, "RasterizerOrderedTexture2DArray",
     TB_SHADER_TEMPLATE_ELEMENT, true, 3},
    {TB_SHADER_OBJECT_RO_TEXTURE3D, "RasterizerOrderedTexture3D", TB_SHADER_TEMPLATE_ELEMENT, true,
     3},
    {TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D, "FeedbackTexture2D", TB_SHADER_TEMPLATE_ELEMENT, true, 0},
    {TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D_ARRAY, "FeedbackTexture2DArray",
     TB_SHADER_TEMPLATE_ELEMENT, true, 0},
    {TB_SHADER_OBJECT_CONSTANT_BUFFER, "ConstantBuffer", TB_SHADER_TEMPLATE_STRUCTURE, false, 0},
    {TB_SHADER_OBJECT_TEXTURE_BUFFER, "TextureBuffer", TB_SHADER_TEMPLATE_STRUCTURE, false, 0},
    {TB_SHADER_OBJECT_ACCELERATION_STRUCTURE, "RaytracingAccelerationStructure",
     TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_RAY_QUERY, "RayQuery", TB_SHADER_TEMPLATE_FLAGS, false, 0},
    {TB_SHADER_OBJECT_HIT_OBJECT, "dx::HitObject", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_INPUT_PATCH, "InputPatch", TB_SHADER_TEMPLATE_PATCH, false, 1},
    {TB_SHADER_OBJECT_OUTPUT_PATCH, "OutputPatch", TB_SHADER_TEMPLATE_PATCH, false, 1},
    {TB_SHADER_OBJECT_POINT_STREAM, "PointStream", TB_SHADER_TEMPLATE_STRUCTURE, false, 0},
    {TB_SHADER_OBJECT_LINE_STREAM, "LineStream", TB_SHADER_TEMPLATE_STRUCTURE, false, 0},
    {TB_SHADER_OBJECT_TRIANGLE_STREAM, "TriangleStream", TB_SHADER_TEMPLATE_STRUCTURE, false, 0},
    {TB_SHADER_OBJECT_STATE_OBJECT_CONFIG, "StateObjectConfig", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_GLOBAL_ROOT_SIGNATURE, "GlobalRootSignature", TB_SHADER_TEMPLATE_NONE, false,
     0},
    {TB_SHADER_OBJECT_LOCAL_ROOT_SIGNATURE, "LocalRootSignature", TB_SHADER_TEMPLATE_NONE, false,
     0},
    {TB_SHADER_OBJECT_SUBOBJECT_TO_EXPORTS_ASSOCIATION, "SubobjectToExportsAssociation",
     TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_RAYTRACING_SHADER_CONFIG, "RaytracingShaderConfig", TB_SHADER_TEMPLATE_NONE,
     false, 0},
    {TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG, "RaytracingPipelineConfig",
     TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG1, "RaytracingPipelineConfig1",
     TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_TRIANGLE_HIT_GROUP, "TriangleHitGroup", TB_SHADER_TEMPLATE_NONE, false, 0},
    {TB_SHADER_OBJECT_PROCEDURAL_PRIMITIVE_HIT_GROUP, "ProceduralPrimitiveHitGroup",
     TB_SHADER_TEMPLATE_NONE, false, 0},
};

const tb_shader_object_info *tb_shader_find_object(tb_shader_name name)
{
    for (size_t index = 0; index < COUNT_OF(objects); index++)
    {
        if (tb_shader_name_equal(
                &name, &(tb_shader_name){objects[index].name, tb_str_length(objects[index].name)}))
        {
            return &objects[index];
        }
    }
    return NULL;
}

const tb_shader_object_info *tb_shader_object_info_of(tb_shader_object object)
{
    for (size_t index = 0; index < COUNT_OF(objects); index++)
    {
        if (objects[index].object == object)
        {
            return &objects[index];
        }
    }
    return NULL;
}

tb_shader_object tb_shader_descriptor_of(tb_shader_object object)
{
    switch (object)
    {
    case TB_SHADER_OBJECT_SAMPLER:
    case TB_SHADER_OBJECT_SAMPLER_COMPARISON:
        return TB_SHADER_OBJECT_SAMPLER_DESCRIPTOR;
    case TB_SHADER_OBJECT_BUFFER:
    case TB_SHADER_OBJECT_RW_BUFFER:
    case TB_SHADER_OBJECT_RO_BUFFER:
    case TB_SHADER_OBJECT_STRUCTURED_BUFFER:
    case TB_SHADER_OBJECT_RW_STRUCTURED_BUFFER:
    case TB_SHADER_OBJECT_RO_STRUCTURED_BUFFER:
    case TB_SHADER_OBJECT_APPEND_STRUCTURED_BUFFER:
    case TB_SHADER_OBJECT_CONSUME_STRUCTURED_BUFFER:
    case TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER:
    case TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER:
    case TB_SHADER_OBJECT_RO_BYTE_ADDRESS_BUFFER:
    case TB_SHADER_OBJECT_TEXTURE1D:
    case TB_SHADER_OBJECT_TEXTURE1D_ARRAY:
    case TB_SHADER_OBJECT_TEXTURE2D:
    case TB_SHADER_OBJECT_TEXTURE2D_ARRAY:
    case TB_SHADER_OBJECT_TEXTURE2DMS:
    case TB_SHADER_OBJECT_TEXTURE2DMS_ARRAY:
    case TB_SHADER_OBJECT_TEXTURE3D:
    case TB_SHADER_OBJECT_TEXTURE_CUBE:
    case TB_SHADER_OBJECT_TEXTURE_CUBE_ARRAY:
    case TB_SHADER_OBJECT_RW_TEXTURE1D:
    case TB_SHADER_OBJECT_RW_TEXTURE1D_ARRAY:
    case TB_SHADER_OBJECT_RW_TEXTURE2D:
    case TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY:
    case TB_SHADER_OBJECT_RW_TEXTURE2DMS:
    case TB_SHADER_OBJECT_RW_TEXTURE2DMS_ARRAY:
    case TB_SHADER_OBJECT_RW_TEXTURE3D:
    case TB_SHADER_OBJECT_RO_TEXTURE1D:
    case TB_SHADER_OBJECT_RO_TEXTURE1D_ARRAY:
    case TB_SHADER_OBJECT_RO_TEXTURE2D:
    case TB_SHADER_OBJECT_RO_TEXTURE2D_ARRAY:
    case TB_SHADER_OBJECT_RO_TEXTURE3D:
    case TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D:
    case TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D_ARRAY:
    case TB_SHADER_OBJECT_CONSTANT_BUFFER:
    case TB_SHADER_OBJECT_TEXTURE_BUFFER:
    case TB_SHADER_OBJECT_ACCELERATION_STRUCTURE:
        return TB_SHADER_OBJECT_RESOURCE_DESCRIPTOR;
    /* The legacy `texture` is an effect's, which DXC ignores. What a shader
     * makes for itself (a RayQuery, a stream, a patch, a texture's members)
     * and a state object's subobjects are in no heap: DXC fails inside when
     * given one from a heap. */
    case TB_SHADER_OBJECT_NONE:
    case TB_SHADER_OBJECT_TEXTURE:
    case TB_SHADER_OBJECT_RAY_QUERY:
    case TB_SHADER_OBJECT_HIT_OBJECT:
    case TB_SHADER_OBJECT_INPUT_PATCH:
    case TB_SHADER_OBJECT_OUTPUT_PATCH:
    case TB_SHADER_OBJECT_POINT_STREAM:
    case TB_SHADER_OBJECT_LINE_STREAM:
    case TB_SHADER_OBJECT_TRIANGLE_STREAM:
    case TB_SHADER_OBJECT_STATE_OBJECT_CONFIG:
    case TB_SHADER_OBJECT_GLOBAL_ROOT_SIGNATURE:
    case TB_SHADER_OBJECT_LOCAL_ROOT_SIGNATURE:
    case TB_SHADER_OBJECT_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
    case TB_SHADER_OBJECT_RAYTRACING_SHADER_CONFIG:
    case TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG:
    case TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG1:
    case TB_SHADER_OBJECT_TRIANGLE_HIT_GROUP:
    case TB_SHADER_OBJECT_PROCEDURAL_PRIMITIVE_HIT_GROUP:
    case TB_SHADER_OBJECT_MIPS:
    case TB_SHADER_OBJECT_MIPS_SLICE:
    case TB_SHADER_OBJECT_SAMPLES:
    case TB_SHADER_OBJECT_SAMPLES_SLICE:
    case TB_SHADER_OBJECT_RESOURCE_HEAP:
    case TB_SHADER_OBJECT_SAMPLER_HEAP:
    case TB_SHADER_OBJECT_RESOURCE_DESCRIPTOR:
    case TB_SHADER_OBJECT_SAMPLER_DESCRIPTOR:
        break;
    }
    return TB_SHADER_OBJECT_NONE;
}

/* A descriptor heap or its element, named as DXC names it; NULL for any
 * other object. */
static const char *heap_name(tb_shader_object object)
{
    static const struct
    {
        tb_shader_object object;
        const char *name;
    } heaps[] = {
        {TB_SHADER_OBJECT_RESOURCE_HEAP, ".Resource"},
        {TB_SHADER_OBJECT_SAMPLER_HEAP, ".Sampler"},
        {TB_SHADER_OBJECT_RESOURCE_DESCRIPTOR, "const .Resource"},
        {TB_SHADER_OBJECT_SAMPLER_DESCRIPTOR, "const .Sampler"},
    };
    for (size_t index = 0; index < COUNT_OF(heaps); index++)
    {
        if (heaps[index].object == object)
        {
            return heaps[index].name;
        }
    }
    return NULL;
}

/* A texture's member, named as DXC names it: `Texture2D<float4>::mips_type`. */
static const char *member_name(tb_shader_types *types, const tb_shader_type *probe)
{
    static const struct
    {
        tb_shader_object object;
        const char *suffix;
    } members[] = {
        {TB_SHADER_OBJECT_MIPS, "mips_type"},
        {TB_SHADER_OBJECT_MIPS_SLICE, "mips_slice_type"},
        {TB_SHADER_OBJECT_SAMPLES, "sample_type"},
        {TB_SHADER_OBJECT_SAMPLES_SLICE, "sample_slice_type"},
    };
    for (size_t index = 0; index < COUNT_OF(members); index++)
    {
        if (members[index].object == probe->object && probe->element != NULL)
        {
            return tb_fmt_string(&types->allocator, "%s::%s", probe->element->name,
                                 members[index].suffix);
        }
    }
    return "(object)";
}

/* `Texture2D<float4>`, `Texture2DMS<float4, 4>`, `RayQuery<5>`, `SamplerState`. */
static const char *object_name(tb_shader_types *types, const tb_shader_type *probe)
{
    const tb_shader_object_info *const info = tb_shader_object_info_of(probe->object);
    if (info == NULL)
    {
        const char *const heap = heap_name(probe->object);
        return heap != NULL ? heap : member_name(types, probe);
    }
    /* An object whose template names an element always has one; without
     * one, it is named as it is written alone. */
    const char *const element = probe->element != NULL ? probe->element->name : NULL;
    switch (info->template_kind)
    {
    case TB_SHADER_TEMPLATE_NONE:
        return info->name;
    case TB_SHADER_TEMPLATE_ELEMENT:
    case TB_SHADER_TEMPLATE_STRUCTURE:
        if (element == NULL)
        {
            return info->name;
        }
        if (probe->second_length != TB_SHADER_NORM_NONE)
        {
            return tb_fmt_string(&types->allocator, "%s<%s %s>", info->name,
                                 probe->second_length == TB_SHADER_NORM_UNORM ? "unorm" : "snorm",
                                 element);
        }
        return tb_fmt_string(&types->allocator, "%s<%s>", info->name, element);
    case TB_SHADER_TEMPLATE_SAMPLES:
    case TB_SHADER_TEMPLATE_PATCH:
        if (element == NULL)
        {
            return info->name;
        }
        if (probe->length == 0)
        {
            return tb_fmt_string(&types->allocator, "%s<%s>", info->name, element);
        }
        return tb_fmt_string(&types->allocator, "%s<%s, %u>", info->name, element,
                             (unsigned)probe->length);
    case TB_SHADER_TEMPLATE_FLAGS:
        if (probe->second_length == 0)
        {
            return tb_fmt_string(&types->allocator, "%s<%u>", info->name, (unsigned)probe->length);
        }
        return tb_fmt_string(&types->allocator, "%s<%u, %u>", info->name, (unsigned)probe->length,
                             (unsigned)probe->second_length);
    }
    return info->name;
}

const tb_shader_type *tb_shader_object_type(tb_shader_types *types, tb_shader_object object,
                                            const tb_shader_type *element, uint32_t length,
                                            uint32_t second_length)
{
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_OBJECT);
    probe.object = object;
    probe.element = element;
    probe.length = length;
    probe.second_length = second_length;
    const tb_shader_type *const found = tb_table_get(types->interned, &probe);
    if (found != NULL)
    {
        return found;
    }
    return intern(types, &probe, object_name(types, &probe));
}

tb_shader_type *tb_shader_struct_type(tb_shader_types *types, tb_shader_name name,
                                      tb_shader_node *declaration)
{
    tb_shader_type *const type = TB_ARENA_NEW(types->arena, tb_shader_type);
    tb_shader_structure *const structure = TB_ARENA_NEW(types->arena, tb_shader_structure);
    type->type_class = TB_SHADER_CLASS_STRUCT;
    type->rows = 1;
    type->columns = 1;
    type->structure = structure;
    type->name = name.length == 0
                     ? "(anonymous struct)"
                     : tb_fmt_string(&types->allocator, "%.*s", (int)name.length, name.text);
    structure->declaration = declaration;
    structure->name = name;
    structure->members =
        tb_table_create(&types->allocator, 8, tb_shader_name_hash, tb_shader_name_equal);
    return type;
}

tb_shader_type *tb_shader_enum_type(tb_shader_types *types, tb_shader_name name,
                                    const tb_shader_node *declaration)
{
    tb_shader_type *const type = TB_ARENA_NEW(types->arena, tb_shader_type);
    type->type_class = TB_SHADER_CLASS_ENUM;
    type->scalar = TB_SHADER_SCALAR_INT;
    type->rows = 1;
    type->columns = 1;
    type->enumeration = declaration;
    type->name = name.length == 0
                     ? "(anonymous enum)"
                     : tb_fmt_string(&types->allocator, "%.*s", (int)name.length, name.text);
    return type;
}

typedef struct builtin_field
{
    const char *name;
    const tb_shader_type *type;
} builtin_field;

static const tb_shader_type *builtin_struct(tb_shader_types *types, const char *name,
                                            const builtin_field *fields, size_t count)
{
    tb_shader_type *const type = tb_shader_struct_type(types, tb_shader_name_of(name), NULL);
    tb_shader_structure *const structure = type->structure;
    structure->fields = TB_ARENA_NEW_ARRAY(types->arena, tb_shader_symbol *, count);
    structure->field_count = count;
    structure->is_complete = true;
    for (size_t index = 0; index < count; index++)
    {
        tb_shader_symbol *const field = TB_ARENA_NEW(types->arena, tb_shader_symbol);
        field->kind = TB_SHADER_SYMBOL_FIELD;
        field->name = tb_shader_name_of(fields[index].name);
        field->type = fields[index].type;
        field->owner = type;
        field->field_index = index;
        structure->fields[index] = field;
        (void)tb_table_put(structure->members, &field->name, field);
    }
    return type;
}

tb_shader_symbol *tb_shader_find_member(const tb_shader_type *type, tb_shader_name name)
{
    for (const tb_shader_type *owner = type; owner != NULL; owner = owner->structure->base)
    {
        tb_shader_symbol *const member = tb_table_get(owner->structure->members, &name);
        if (member != NULL)
        {
            return member;
        }
    }
    return NULL;
}

const tb_shader_type *tb_shader_heap_type(tb_shader_types *types, tb_shader_object heap)
{
    const tb_shader_object descriptor = heap == TB_SHADER_OBJECT_SAMPLER_HEAP
                                            ? TB_SHADER_OBJECT_SAMPLER_DESCRIPTOR
                                            : TB_SHADER_OBJECT_RESOURCE_DESCRIPTOR;
    return tb_shader_object_type(types, heap, tb_shader_object_type(types, descriptor, NULL, 0, 0),
                                 0, 0);
}

const tb_shader_type *tb_shader_ray_desc_type(tb_shader_types *types)
{
    return types->ray_desc;
}

const tb_shader_type *tb_shader_triangle_attributes_type(tb_shader_types *types)
{
    return types->triangle_attributes;
}

/* -------------------------------------------------------------------------
 * The table.
 * ------------------------------------------------------------------------- */

tb_shader_types *tb_shader_types_create(tb_arena *arena)
{
    tb_shader_types *const types = TB_ARENA_NEW(arena, tb_shader_types);
    types->arena = arena;
    types->allocator = tb_arena_allocator(arena);
    types->interned = tb_table_create(&types->allocator, 64, type_hash, type_equal);
    tb_shader_type probe = new_probe(TB_SHADER_CLASS_ERROR);
    types->error = intern(types, &probe, "(error)");
    probe = new_probe(TB_SHADER_CLASS_VOID);
    types->void_type = intern(types, &probe, "void");
    probe = new_probe(TB_SHADER_CLASS_STRING);
    types->string = intern(types, &probe, "string");
    const tb_shader_type *const float1 = tb_shader_scalar_type(types, TB_SHADER_SCALAR_FLOAT);
    const tb_shader_type *const float3 = tb_shader_vector_type(types, TB_SHADER_SCALAR_FLOAT, 3);
    const builtin_field ray_fields[] = {
        {"Origin", float3}, {"TMin", float1}, {"Direction", float3}, {"TMax", float1}};
    types->ray_desc = builtin_struct(types, "RayDesc", ray_fields, COUNT_OF(ray_fields));
    const builtin_field attribute_fields[] = {
        {"barycentrics", tb_shader_vector_type(types, TB_SHADER_SCALAR_FLOAT, 2)}};
    types->triangle_attributes = builtin_struct(types, "BuiltInTriangleIntersectionAttributes",
                                                attribute_fields, COUNT_OF(attribute_fields));
    return types;
}

const tb_shader_type *tb_shader_error_type(tb_shader_types *types)
{
    return types->error;
}

const tb_shader_type *tb_shader_void_type(tb_shader_types *types)
{
    return types->void_type;
}

const tb_shader_type *tb_shader_string_type(tb_shader_types *types)
{
    return types->string;
}

/* -------------------------------------------------------------------------
 * Classes and ranks.
 * ------------------------------------------------------------------------- */

bool tb_shader_scalar_is_integer(tb_shader_scalar scalar)
{
    switch (scalar)
    {
    case TB_SHADER_SCALAR_INT:
    case TB_SHADER_SCALAR_UINT:
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_UINT16:
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_UINT64:
    case TB_SHADER_SCALAR_MIN16INT:
    case TB_SHADER_SCALAR_MIN16UINT:
    case TB_SHADER_SCALAR_LITERAL_INT:
        return true;
    case TB_SHADER_SCALAR_BOOL:
    case TB_SHADER_SCALAR_MIN16FLOAT:
    case TB_SHADER_SCALAR_HALF:
    case TB_SHADER_SCALAR_FLOAT:
    case TB_SHADER_SCALAR_DOUBLE:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
        return false;
    }
    return false;
}

bool tb_shader_scalar_is_float(tb_shader_scalar scalar)
{
    return scalar != TB_SHADER_SCALAR_BOOL && !tb_shader_scalar_is_integer(scalar);
}

bool tb_shader_scalar_is_literal(tb_shader_scalar scalar)
{
    return scalar == TB_SHADER_SCALAR_LITERAL_INT || scalar == TB_SHADER_SCALAR_LITERAL_FLOAT;
}

bool tb_shader_type_is_numeric(const tb_shader_type *type)
{
    return type->type_class == TB_SHADER_CLASS_SCALAR ||
           type->type_class == TB_SHADER_CLASS_VECTOR || type->type_class == TB_SHADER_CLASS_MATRIX;
}

uint32_t tb_shader_component_count(const tb_shader_type *type)
{
    return type->rows * type->columns;
}

uint32_t tb_shader_flat_count(const tb_shader_type *type)
{
    switch (type->type_class)
    {
    case TB_SHADER_CLASS_SCALAR:
    case TB_SHADER_CLASS_VECTOR:
    case TB_SHADER_CLASS_MATRIX:
    case TB_SHADER_CLASS_ENUM:
        return tb_shader_component_count(type);
    case TB_SHADER_CLASS_ARRAY: {
        const uint32_t element = tb_shader_flat_count(type->element);
        if (element == 0 || type->length == 0 || element > UINT32_MAX / type->length)
        {
            return 0;
        }
        return element * type->length;
    }
    case TB_SHADER_CLASS_STRUCT: {
        uint32_t total = 0;
        const tb_shader_structure *const structure = type->structure;
        for (size_t index = 0; index < structure->field_count; index++)
        {
            const uint32_t field = tb_shader_flat_count(structure->fields[index]->type);
            if (field == 0 || field > UINT32_MAX - total)
            {
                return 0;
            }
            total += field;
        }
        return total;
    }
    case TB_SHADER_CLASS_ERROR:
    case TB_SHADER_CLASS_VOID:
    case TB_SHADER_CLASS_OBJECT:
    case TB_SHADER_CLASS_STRING:
        return 0;
    }
    return 0;
}

tb_shader_scalar tb_shader_concrete_scalar(tb_shader_scalar scalar)
{
    if (scalar == TB_SHADER_SCALAR_LITERAL_INT)
    {
        return TB_SHADER_SCALAR_INT;
    }
    if (scalar == TB_SHADER_SCALAR_LITERAL_FLOAT)
    {
        return TB_SHADER_SCALAR_FLOAT;
    }
    return scalar;
}

/* Higher ranks win in arithmetic: floating point over integers, wider over
 * narrower, and unsigned over signed of the same width. */
static int scalar_rank(tb_shader_scalar scalar)
{
    static const int ranks[TB_SHADER_SCALAR_COUNT] = {
        [TB_SHADER_SCALAR_BOOL] = 0,
        [TB_SHADER_SCALAR_LITERAL_INT] = 1,
        [TB_SHADER_SCALAR_MIN16INT] = 2,
        [TB_SHADER_SCALAR_MIN16UINT] = 3,
        [TB_SHADER_SCALAR_INT16] = 4,
        [TB_SHADER_SCALAR_UINT16] = 5,
        [TB_SHADER_SCALAR_INT] = 6,
        [TB_SHADER_SCALAR_UINT] = 7,
        [TB_SHADER_SCALAR_INT64] = 8,
        [TB_SHADER_SCALAR_UINT64] = 9,
        [TB_SHADER_SCALAR_LITERAL_FLOAT] = 10,
        [TB_SHADER_SCALAR_MIN16FLOAT] = 11,
        [TB_SHADER_SCALAR_HALF] = 12,
        [TB_SHADER_SCALAR_FLOAT] = 13,
        [TB_SHADER_SCALAR_DOUBLE] = 14,
    };
    return ranks[scalar];
}

tb_shader_scalar tb_shader_common_scalar(tb_shader_scalar left, tb_shader_scalar right)
{
    /* A literal takes the other operand's type, except that a floating-point
     * literal makes an integer operand float. */
    if (tb_shader_scalar_is_literal(left) && !tb_shader_scalar_is_literal(right))
    {
        const tb_shader_scalar swapped = left;
        left = right;
        right = swapped;
    }
    if (right == TB_SHADER_SCALAR_LITERAL_INT && !tb_shader_scalar_is_literal(left))
    {
        return left == TB_SHADER_SCALAR_BOOL ? TB_SHADER_SCALAR_INT : left;
    }
    if (right == TB_SHADER_SCALAR_LITERAL_FLOAT && !tb_shader_scalar_is_literal(left))
    {
        return tb_shader_scalar_is_float(left) ? left : TB_SHADER_SCALAR_FLOAT;
    }
    /* Bools take part in arithmetic as ints. */
    if (left == TB_SHADER_SCALAR_BOOL && right == TB_SHADER_SCALAR_BOOL)
    {
        return TB_SHADER_SCALAR_INT;
    }
    return scalar_rank(left) >= scalar_rank(right) ? left : right;
}
