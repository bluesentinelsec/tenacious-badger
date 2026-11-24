/* The shader compiler's types: what a value is, as type checking decides it.
 *
 * Types are interned: each type exists once in a type table, so two types
 * are the same type when their pointers are equal. A table and its types
 * live in the arena it was created in. */
#ifndef TB_SHADER_TYPES_H
#define TB_SHADER_TYPES_H

#include "shader_syntax.h"

#include "badger/arena.h"
#include "badger/table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The component of a scalar, a vector or a matrix. */
typedef enum tb_shader_scalar
{
    TB_SHADER_SCALAR_BOOL,
    TB_SHADER_SCALAR_INT,
    TB_SHADER_SCALAR_UINT,
    TB_SHADER_SCALAR_INT16,
    TB_SHADER_SCALAR_UINT16,
    TB_SHADER_SCALAR_INT64,
    TB_SHADER_SCALAR_UINT64,
    TB_SHADER_SCALAR_MIN16INT,
    TB_SHADER_SCALAR_MIN16UINT,
    TB_SHADER_SCALAR_MIN16FLOAT,
    TB_SHADER_SCALAR_HALF,
    TB_SHADER_SCALAR_FLOAT,
    TB_SHADER_SCALAR_DOUBLE,
    /* The type of an integer or floating-point literal without a suffix,
     * which takes the type its use asks for, as in DXC. */
    TB_SHADER_SCALAR_LITERAL_INT,
    TB_SHADER_SCALAR_LITERAL_FLOAT
} tb_shader_scalar;

#define TB_SHADER_SCALAR_COUNT ((size_t)TB_SHADER_SCALAR_LITERAL_FLOAT + 1)

typedef enum tb_shader_type_class
{
    /* A type that could not be decided, after an error was reported. Every
     * use of it is accepted, so one mistake is reported once. */
    TB_SHADER_CLASS_ERROR,
    TB_SHADER_CLASS_VOID,
    TB_SHADER_CLASS_SCALAR,
    TB_SHADER_CLASS_VECTOR,
    TB_SHADER_CLASS_MATRIX,
    TB_SHADER_CLASS_ARRAY,
    TB_SHADER_CLASS_STRUCT,
    TB_SHADER_CLASS_ENUM,
    TB_SHADER_CLASS_OBJECT, /* A resource, a sampler, or another built-in object. */
    TB_SHADER_CLASS_STRING
} tb_shader_type_class;

/* The built-in objects. */
typedef enum tb_shader_object
{
    TB_SHADER_OBJECT_NONE,
    TB_SHADER_OBJECT_SAMPLER,
    TB_SHADER_OBJECT_SAMPLER_COMPARISON,
    TB_SHADER_OBJECT_TEXTURE, /* The legacy `texture`. */
    TB_SHADER_OBJECT_BUFFER,
    TB_SHADER_OBJECT_RW_BUFFER,
    TB_SHADER_OBJECT_RO_BUFFER,
    TB_SHADER_OBJECT_STRUCTURED_BUFFER,
    TB_SHADER_OBJECT_RW_STRUCTURED_BUFFER,
    TB_SHADER_OBJECT_RO_STRUCTURED_BUFFER,
    TB_SHADER_OBJECT_APPEND_STRUCTURED_BUFFER,
    TB_SHADER_OBJECT_CONSUME_STRUCTURED_BUFFER,
    TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER,
    TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER,
    TB_SHADER_OBJECT_RO_BYTE_ADDRESS_BUFFER,
    TB_SHADER_OBJECT_TEXTURE1D,
    TB_SHADER_OBJECT_TEXTURE1D_ARRAY,
    TB_SHADER_OBJECT_TEXTURE2D,
    TB_SHADER_OBJECT_TEXTURE2D_ARRAY,
    TB_SHADER_OBJECT_TEXTURE2DMS,
    TB_SHADER_OBJECT_TEXTURE2DMS_ARRAY,
    TB_SHADER_OBJECT_TEXTURE3D,
    TB_SHADER_OBJECT_TEXTURE_CUBE,
    TB_SHADER_OBJECT_TEXTURE_CUBE_ARRAY,
    TB_SHADER_OBJECT_RW_TEXTURE1D,
    TB_SHADER_OBJECT_RW_TEXTURE1D_ARRAY,
    TB_SHADER_OBJECT_RW_TEXTURE2D,
    TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY,
    TB_SHADER_OBJECT_RW_TEXTURE2DMS,
    TB_SHADER_OBJECT_RW_TEXTURE2DMS_ARRAY,
    TB_SHADER_OBJECT_RW_TEXTURE3D,
    TB_SHADER_OBJECT_RO_TEXTURE1D,
    TB_SHADER_OBJECT_RO_TEXTURE1D_ARRAY,
    TB_SHADER_OBJECT_RO_TEXTURE2D,
    TB_SHADER_OBJECT_RO_TEXTURE2D_ARRAY,
    TB_SHADER_OBJECT_RO_TEXTURE3D,
    TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D,
    TB_SHADER_OBJECT_FEEDBACK_TEXTURE2D_ARRAY,
    TB_SHADER_OBJECT_CONSTANT_BUFFER,
    TB_SHADER_OBJECT_TEXTURE_BUFFER,
    TB_SHADER_OBJECT_ACCELERATION_STRUCTURE,
    TB_SHADER_OBJECT_RAY_QUERY,
    TB_SHADER_OBJECT_HIT_OBJECT,
    TB_SHADER_OBJECT_INPUT_PATCH,
    TB_SHADER_OBJECT_OUTPUT_PATCH,
    TB_SHADER_OBJECT_POINT_STREAM,
    TB_SHADER_OBJECT_LINE_STREAM,
    TB_SHADER_OBJECT_TRIANGLE_STREAM,
    /* The subobjects of a ray-tracing state object, declared in HLSL. */
    TB_SHADER_OBJECT_STATE_OBJECT_CONFIG,
    TB_SHADER_OBJECT_GLOBAL_ROOT_SIGNATURE,
    TB_SHADER_OBJECT_LOCAL_ROOT_SIGNATURE,
    TB_SHADER_OBJECT_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
    TB_SHADER_OBJECT_RAYTRACING_SHADER_CONFIG,
    TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG,
    TB_SHADER_OBJECT_RAYTRACING_PIPELINE_CONFIG1,
    TB_SHADER_OBJECT_TRIANGLE_HIT_GROUP,
    TB_SHADER_OBJECT_PROCEDURAL_PRIMITIVE_HIT_GROUP,
    /* A texture's `mips` and `sample` members, and a mip level or a sample of
     * them, which only those members make; their element is the texture. */
    TB_SHADER_OBJECT_MIPS,
    TB_SHADER_OBJECT_MIPS_SLICE,
    TB_SHADER_OBJECT_SAMPLES,
    TB_SHADER_OBJECT_SAMPLES_SLICE,
    /* Shader Model 6.6's descriptor heaps, `ResourceDescriptorHeap` and
     * `SamplerDescriptorHeap`, named as DXC names them (`.Resource`,
     * `.Sampler`); and an element of one (`const .Resource`), which only
     * indexing a heap makes, and which converts to any resource, or any
     * sampler. A heap's element is its descriptor. */
    TB_SHADER_OBJECT_RESOURCE_HEAP,
    TB_SHADER_OBJECT_SAMPLER_HEAP,
    TB_SHADER_OBJECT_RESOURCE_DESCRIPTOR,
    TB_SHADER_OBJECT_SAMPLER_DESCRIPTOR
} tb_shader_object;

/* What an object's template arguments are. */
typedef enum tb_shader_object_template
{
    TB_SHADER_TEMPLATE_NONE,      /* `SamplerState` */
    TB_SHADER_TEMPLATE_ELEMENT,   /* `Texture2D<float4>`, and `Texture2D` is `<float4>` */
    TB_SHADER_TEMPLATE_STRUCTURE, /* `StructuredBuffer<T>`: required */
    TB_SHADER_TEMPLATE_SAMPLES,   /* `Texture2DMS<float4, 4>`: element, and optional samples */
    TB_SHADER_TEMPLATE_PATCH,     /* `InputPatch<T, 3>`: both required */
    TB_SHADER_TEMPLATE_FLAGS      /* `RayQuery<flags>`, and optional second flags */
} tb_shader_object_template;

/* What is known of a built-in object. */
typedef struct tb_shader_object_info
{
    tb_shader_object object;
    const char *name;
    tb_shader_object_template template_kind;
    bool is_writable; /* A UAV. */
    uint32_t
        index_dimensions; /* `object[i]` takes this many components; 0 when it cannot be indexed. */
} tb_shader_object_info;

/* The built-in object a name declares, or NULL. */
const tb_shader_object_info *tb_shader_find_object(tb_shader_name name);
const tb_shader_object_info *tb_shader_object_info_of(tb_shader_object object);

/* The descriptor an object is made from, indexing a descriptor heap:
 * RESOURCE_DESCRIPTOR for a texture, a buffer, a constant or texture buffer
 * and an acceleration structure; SAMPLER_DESCRIPTOR for a sampler; NONE for
 * any other object, which no heap holds. */
tb_shader_object tb_shader_descriptor_of(tb_shader_object object);

typedef struct tb_shader_type tb_shader_type;
typedef struct tb_shader_symbol tb_shader_symbol;

/* A struct's members: its fields in order, its base's first. */
typedef struct tb_shader_structure
{
    tb_shader_node *declaration; /* NULL for a built-in struct such as RayDesc. */
    tb_shader_name name;         /* Qualified: `Lighting::Light`. */
    const tb_shader_type *base;
    tb_shader_symbol **fields;
    size_t field_count;
    tb_table *members; /* Fields and methods by name, the base's included. */
    bool is_complete;  /* Its body has been read. */
} tb_shader_structure;

struct tb_shader_type
{
    tb_shader_type_class type_class;
    tb_shader_scalar scalar;       /* SCALAR, VECTOR, MATRIX; ENUM: its underlying scalar. */
    uint32_t rows;                 /* MATRIX; 1 otherwise. */
    uint32_t columns;              /* VECTOR: its components; MATRIX; 1 for a scalar. */
    const tb_shader_type *element; /* ARRAY: the element; OBJECT: its template's type, or NULL. */
    uint32_t length; /* ARRAY: elements, 0 when unsized; OBJECT: samples, patch size, flags. */
    uint32_t second_length;            /* OBJECT: RayQuery's second flags. */
    tb_shader_object object;           /* OBJECT */
    tb_shader_structure *structure;    /* STRUCT */
    const tb_shader_node *enumeration; /* ENUM: its declaration. */
    const char *name; /* As DXC writes it: `float4`, `Texture2D<float4>`, `int[4]`. */
};

typedef enum tb_shader_symbol_kind
{
    TB_SHADER_SYMBOL_VARIABLE,
    TB_SHADER_SYMBOL_PARAMETER,
    TB_SHADER_SYMBOL_FUNCTION,
    TB_SHADER_SYMBOL_FIELD,
    TB_SHADER_SYMBOL_TYPE,
    TB_SHADER_SYMBOL_ENUMERATOR,
    TB_SHADER_SYMBOL_NAMESPACE
} tb_shader_symbol_kind;

/* Where a variable lives. */
typedef enum tb_shader_storage
{
    TB_SHADER_STORAGE_LOCAL,
    TB_SHADER_STORAGE_STATIC, /* `static`, at namespace scope or in a function. */
    TB_SHADER_STORAGE_GROUPSHARED,
    TB_SHADER_STORAGE_UNIFORM, /* A global that is not static: a constant or a resource. */
    TB_SHADER_STORAGE_PARAMETER
} tb_shader_storage;

/* What a name names. */
struct tb_shader_symbol
{
    tb_shader_symbol_kind kind;
    tb_shader_name name;         /* Qualified at namespace scope: `Lighting::Falloff`. */
    const tb_shader_type *type;  /* A function's: what it returns. */
    tb_shader_node *declaration; /* NULL for what is built in. */
    uint64_t modifiers;
    tb_shader_storage storage;
    bool is_const;
    bool has_value; /* An enumerator, or a constant initialized with a constant. */
    tb_shader_constant value;
    tb_shader_symbol *next_overload; /* FUNCTION: the next function of the same name. */
    const tb_shader_type *owner;     /* FIELD, and a method: its struct. */
    size_t field_index;              /* FIELD */
    tb_shader_node *buffer;          /* VARIABLE: the cbuffer or tbuffer it is in. */
    bool is_defined;                 /* FUNCTION: has a body. */
    size_t scope_depth; /* How deep in a function's scopes it was declared; 0 outside. */
};

/* Hash and equality of tb_shader_name keys, for tb_table. */
uint64_t tb_shader_name_hash(const void *key);
bool tb_shader_name_equal(const void *left, const void *right);
tb_shader_name tb_shader_name_of(const char *text);

/* Every type of one compile. */
typedef struct tb_shader_types tb_shader_types;

tb_shader_types *tb_shader_types_create(tb_arena *arena);

const tb_shader_type *tb_shader_error_type(tb_shader_types *types);
const tb_shader_type *tb_shader_void_type(tb_shader_types *types);
const tb_shader_type *tb_shader_string_type(tb_shader_types *types);
const tb_shader_type *tb_shader_scalar_type(tb_shader_types *types, tb_shader_scalar scalar);
/* A vector of 1 to 4 components; a vector of 1 is a type of its own, as in HLSL. */
const tb_shader_type *tb_shader_vector_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            uint32_t columns);
/* A matrix of 1 to 4 rows and columns. */
const tb_shader_type *tb_shader_matrix_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            uint32_t rows, uint32_t columns);
/* A scalar, a vector or a matrix of `scalar` shaped as `shape`. */
const tb_shader_type *tb_shader_shaped_type(tb_shader_types *types, tb_shader_scalar scalar,
                                            const tb_shader_type *shape);
/* An array; `length` 0 is unsized. */
const tb_shader_type *tb_shader_array_type(tb_shader_types *types, const tb_shader_type *element,
                                           uint32_t length);
const tb_shader_type *tb_shader_object_type(tb_shader_types *types, tb_shader_object object,
                                            const tb_shader_type *element, uint32_t length,
                                            uint32_t second_length);
/* A new struct, which is not interned: each declaration is a type of its own. */
tb_shader_type *tb_shader_struct_type(tb_shader_types *types, tb_shader_name name,
                                      tb_shader_node *declaration);
tb_shader_type *tb_shader_enum_type(tb_shader_types *types, tb_shader_name name,
                                    const tb_shader_node *declaration);
/* A struct's field or method by name, its bases' included, or NULL. */
tb_shader_symbol *tb_shader_find_member(const tb_shader_type *type, tb_shader_name name);
/* A descriptor heap, RESOURCE_HEAP or SAMPLER_HEAP; its element is its descriptor. */
const tb_shader_type *tb_shader_heap_type(tb_shader_types *types, tb_shader_object heap);
/* The built-in structs: RayDesc and BuiltInTriangleIntersectionAttributes. */
const tb_shader_type *tb_shader_ray_desc_type(tb_shader_types *types);
const tb_shader_type *tb_shader_triangle_attributes_type(tb_shader_types *types);

/* The type a built-in scalar, vector or matrix name names (`float4`,
 * `min16uint2x3`, `dword`), or NULL. */
const tb_shader_type *tb_shader_numeric_type(tb_shader_types *types, tb_shader_name name);

/* Classes of scalars. */
bool tb_shader_scalar_is_integer(tb_shader_scalar scalar); /* bool excluded */
bool tb_shader_scalar_is_float(tb_shader_scalar scalar);
bool tb_shader_scalar_is_literal(tb_shader_scalar scalar);
bool tb_shader_type_is_numeric(const tb_shader_type *type); /* Scalar, vector or matrix. */
/* Components of a scalar, vector or matrix. */
uint32_t tb_shader_component_count(const tb_shader_type *type);
/* Scalar components a value of the type holds, flattened as HLSL's initializer
 * lists and casts flatten it; 0 when it holds something that is not one. */
uint32_t tb_shader_flat_count(const tb_shader_type *type);
/* The scalar a literal takes when nothing else decides: int, or float. */
tb_shader_scalar tb_shader_concrete_scalar(tb_shader_scalar scalar);
/* A constant converted from one scalar type to another, as HLSL converts
 * (shader_check_constant.c). */
tb_shader_constant tb_shader_constant_convert(tb_shader_constant constant, tb_shader_scalar from,
                                              tb_shader_scalar to);
/* The scalar two operands of an arithmetic operator are converted to. */
tb_shader_scalar tb_shader_common_scalar(tb_shader_scalar left, tb_shader_scalar right);

#endif
