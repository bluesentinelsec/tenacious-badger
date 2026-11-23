/* Checking calls: of the shader's functions and methods, and of HLSL's
 * intrinsic functions, whose signatures are templates the arguments decide
 * (shader_intrinsics.h). Of the overloads a call could mean, it takes the
 * one no worse for any argument and better for one, as DXC does. */
#include "shader_check_internal.h"
#include "shader_intrinsics.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/str.h"

#include <string.h>

#define MAX_ARGUMENTS 64

static int name_length(tb_shader_name name)
{
    return (int)name.length;
}

static bool is_error(const tb_shader_type *type)
{
    return type == NULL || type->type_class == TB_SHADER_CLASS_ERROR;
}

static const tb_shader_type *type_of(tb_shader_checker *checker, const tb_shader_node *node)
{
    return node == NULL || node->checked == NULL ? tb_shader_error_type(checker->types)
                                                 : node->checked;
}

/* What a call passes to and gets from one overload. */
typedef struct signature
{
    const tb_shader_type *result;
    const tb_shader_type *parameters[MAX_ARGUMENTS];
    bool is_output[MAX_ARGUMENTS];
    size_t count;
    size_t required;  /* Parameters without a default value. */
    bool is_variadic; /* Takes further arguments, as they are. */
} signature;

/* -------------------------------------------------------------------------
 * Reading an intrinsic's signatures.
 * ------------------------------------------------------------------------- */

/* Templates `#1` to `#4` and shapes `a1` to `a4`. */
#define MAX_TEMPLATES 4
#define SIZE_LETTERS 26
#define MAX_INTRINSIC_PARAMETERS 12

typedef enum direction
{
    DIRECTION_IN,
    DIRECTION_OUT,
    DIRECTION_INOUT
} direction;

typedef enum component_kind
{
    COMPONENT_SCALAR,          /* `float` */
    COMPONENT_TEMPLATE,        /* `numeric#1`, `#1` */
    COMPONENT_STRUCT_TEMPLATE, /* `udt#1` */
    COMPONENT_TYPE             /* `RayDesc`: a whole type */
} component_kind;

typedef enum component_class
{
    CLASS_NONE, /* A use of a template, which its declaration classifies. */
    CLASS_NUMERIC,
    CLASS_FLOAT,
    CLASS_FDOUBLE,
    CLASS_DOUBLE,
    CLASS_INT,
    CLASS_UINT,
    CLASS_ATOMIC,
    CLASS_B32,
    CLASS_B16,
    CLASS_ANY
} component_class;

typedef enum shape_kind
{
    SHAPE_SCALAR,
    SHAPE_TEMPLATE, /* `<a1>` */
    SHAPE_VECTOR,   /* `<3>`, `<N>` */
    SHAPE_MATRIX    /* `<3x4>`, `<RxC>` */
} shape_kind;

/* A size of a vector or a matrix: a number, or a letter the arguments decide. */
typedef struct size_term
{
    uint32_t value;
    int letter; /* -1 for a number. */
} size_term;

typedef struct term
{
    direction direction;
    component_kind component;
    tb_shader_scalar scalar;    /* SCALAR */
    const tb_shader_type *type; /* TYPE */
    size_t template_index;      /* TEMPLATE, STRUCT_TEMPLATE */
    component_class class_of;   /* TEMPLATE: where it is declared. */
    shape_kind shape;
    size_t shape_index; /* SHAPE_TEMPLATE */
    char layout;        /* SHAPE_TEMPLATE: 'a', 'v' or 'm' */
    size_term rows;     /* MATRIX */
    size_term columns;  /* VECTOR, MATRIX */
} term;

typedef struct parsed_signature
{
    term result;
    term parameters[MAX_INTRINSIC_PARAMETERS];
    size_t count;
    bool is_variadic;
} parsed_signature;

typedef struct reader
{
    tb_shader_types *types;
    const char *text;
    size_t at;
} reader;

static void skip_spaces(reader *in)
{
    while (in->text[in->at] == ' ')
    {
        in->at++;
    }
}

static bool accept(reader *in, char expected)
{
    skip_spaces(in);
    if (in->text[in->at] != expected)
    {
        return false;
    }
    in->at++;
    return true;
}

static bool is_word_byte(char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

static tb_shader_name read_word(reader *in)
{
    skip_spaces(in);
    const size_t start = in->at;
    while (is_word_byte(in->text[in->at]))
    {
        in->at++;
    }
    const tb_shader_name word = {in->text + start, in->at - start};
    return word;
}

static bool word_is(tb_shader_name word, const char *text)
{
    return word.length == tb_str_length(text) && memcmp(word.text, text, word.length) == 0;
}

/* A template's number, `1` to `4`, as an index. */
static bool read_template_index(reader *in, size_t *index)
{
    const char byte = in->text[in->at];
    if (byte < '1' || byte > '0' + MAX_TEMPLATES)
    {
        return false;
    }
    *index = (size_t)(byte - '1');
    in->at++;
    return true;
}

static component_class class_named(tb_shader_name word)
{
    static const struct
    {
        const char *name;
        component_class class_of;
    } classes[] = {
        {"numeric", CLASS_NUMERIC}, {"float", CLASS_FLOAT}, {"fdouble", CLASS_FDOUBLE},
        {"double", CLASS_DOUBLE},   {"int", CLASS_INT},     {"uint", CLASS_UINT},
        {"atomic", CLASS_ATOMIC},   {"b32", CLASS_B32},     {"b16", CLASS_B16},
        {"any", CLASS_ANY},
    };
    for (size_t index = 0; index < sizeof(classes) / sizeof(classes[0]); index++)
    {
        if (word_is(word, classes[index].name))
        {
            return classes[index].class_of;
        }
    }
    return CLASS_NONE;
}

/* A type named by a word: `void`, `string`, `RayDesc`, or an object without
 * template arguments. */
static const tb_shader_type *type_named(tb_shader_types *types, tb_shader_name word)
{
    if (word_is(word, "void"))
    {
        return tb_shader_void_type(types);
    }
    if (word_is(word, "string"))
    {
        return tb_shader_string_type(types);
    }
    if (word_is(word, "RayDesc"))
    {
        return tb_shader_ray_desc_type(types);
    }
    const tb_shader_object_info *const info = tb_shader_find_object(word);
    if (info == NULL || info->template_kind != TB_SHADER_TEMPLATE_NONE)
    {
        return NULL;
    }
    return tb_shader_object_type(types, info->object, NULL, 0, 0);
}

static bool read_size(reader *in, size_term *size)
{
    const char byte = in->text[in->at];
    if (byte >= '1' && byte <= '4')
    {
        size->value = (uint32_t)(byte - '0');
        size->letter = -1;
        in->at++;
        return true;
    }
    if (byte >= 'A' && byte <= 'Z')
    {
        size->value = 0;
        size->letter = byte - 'A';
        in->at++;
        return true;
    }
    return false;
}

static bool read_shape(reader *in, term *out)
{
    skip_spaces(in);
    const char byte = in->text[in->at];
    if (byte == 'a' || byte == 'v' || byte == 'm')
    {
        in->at++;
        out->shape = SHAPE_TEMPLATE;
        out->layout = byte;
        return read_template_index(in, &out->shape_index);
    }
    if (!read_size(in, &out->columns))
    {
        return false;
    }
    out->shape = SHAPE_VECTOR;
    if (in->text[in->at] != 'x')
    {
        return true;
    }
    in->at++;
    out->shape = SHAPE_MATRIX;
    out->rows = out->columns;
    return read_size(in, &out->columns);
}

static bool read_direction(reader *in, term *out)
{
    const size_t start = in->at;
    const tb_shader_name word = read_word(in);
    if (word_is(word, "out") && in->text[in->at] == ' ')
    {
        out->direction = DIRECTION_OUT;
        return true;
    }
    if (word_is(word, "inout") && in->text[in->at] == ' ')
    {
        out->direction = DIRECTION_INOUT;
        return true;
    }
    in->at = start;
    return false;
}

/* The component of a term: `$1`, `#1`, `class#1`, `udt#1`, a scalar or a type. */
static bool read_component(reader *in, term *out)
{
    if (accept(in, '$'))
    {
        out->component = COMPONENT_TEMPLATE;
        out->shape = SHAPE_TEMPLATE;
        if (!read_template_index(in, &out->template_index))
        {
            return false;
        }
        out->shape_index = out->template_index;
        return true;
    }
    if (accept(in, '#'))
    {
        out->component = COMPONENT_TEMPLATE;
        return read_template_index(in, &out->template_index);
    }
    const tb_shader_name word = read_word(in);
    if (word.length == 0)
    {
        return false;
    }
    if (in->text[in->at] == '#')
    {
        in->at++;
        if (word_is(word, "udt"))
        {
            out->component = COMPONENT_STRUCT_TEMPLATE;
            return read_template_index(in, &out->template_index);
        }
        out->component = COMPONENT_TEMPLATE;
        out->class_of = class_named(word);
        return out->class_of != CLASS_NONE && read_template_index(in, &out->template_index);
    }
    const tb_shader_type *const numeric = tb_shader_numeric_type(in->types, word);
    if (numeric != NULL && numeric->type_class == TB_SHADER_CLASS_SCALAR)
    {
        out->component = COMPONENT_SCALAR;
        out->scalar = numeric->scalar;
        return true;
    }
    out->component = COMPONENT_TYPE;
    out->type = type_named(in->types, word);
    return out->type != NULL;
}

static bool read_term(reader *in, term *out, bool is_parameter)
{
    const term empty = {0};
    *out = empty;
    if (is_parameter)
    {
        (void)read_direction(in, out);
    }
    if (!read_component(in, out))
    {
        return false;
    }
    if (!accept(in, '<'))
    {
        return true;
    }
    const bool takes_shape = out->component == COMPONENT_SCALAR ||
                             (out->component == COMPONENT_TEMPLATE && out->shape == SHAPE_SCALAR);
    return takes_shape && read_shape(in, out) && accept(in, '>');
}

/* One signature, from where `in` is to its `;` or the end. */
static bool read_signature(reader *in, parsed_signature *out)
{
    const parsed_signature empty = {0};
    *out = empty;
    if (!read_term(in, &out->result, false) || !accept(in, '('))
    {
        return false;
    }
    if (accept(in, ')'))
    {
        return true;
    }
    do
    {
        skip_spaces(in);
        if (tb_str_matches_at(in->text, in->at, "..."))
        {
            in->at += 3;
            out->is_variadic = true;
            break;
        }
        if (out->count == MAX_INTRINSIC_PARAMETERS ||
            !read_term(in, &out->parameters[out->count], true))
        {
            return false;
        }
        out->count++;
    } while (accept(in, ','));
    if (!accept(in, ')'))
    {
        return false;
    }
    skip_spaces(in);
    return in->text[in->at] == '\0' || accept(in, ';');
}

bool tb_shader_check_intrinsic_is_valid(tb_shader_types *types, tb_shader_intrinsic intrinsic)
{
    const tb_shader_intrinsic_info *const info = tb_shader_intrinsic_info_of(intrinsic);
    if (info == NULL)
    {
        return false;
    }
    reader in = {types, info->signatures, 0};
    parsed_signature parsed = {0};
    do
    {
        if (!read_signature(&in, &parsed))
        {
            return false;
        }
    } while (in.text[in.at] != '\0');
    return true;
}

/* -------------------------------------------------------------------------
 * Deciding an intrinsic's templates from the arguments.
 * ------------------------------------------------------------------------- */

typedef struct shape_value
{
    bool is_set;
    tb_shader_type_class type_class; /* SCALAR, VECTOR or MATRIX */
    uint32_t rows;
    uint32_t columns;
} shape_value;

typedef struct bindings
{
    component_class classes[MAX_TEMPLATES];
    bool has_component[MAX_TEMPLATES];
    tb_shader_scalar components[MAX_TEMPLATES];
    const tb_shader_type *structs[MAX_TEMPLATES];
    char layouts[MAX_TEMPLATES];
    shape_value shapes[MAX_TEMPLATES];
    uint32_t sizes[SIZE_LETTERS]; /* 0 until an argument gives one. */
} bindings;

static bool is_in_class(component_class class_of, tb_shader_scalar scalar)
{
    switch (class_of)
    {
    case CLASS_NONE:
    case CLASS_ANY:
        return true;
    case CLASS_NUMERIC:
        return scalar != TB_SHADER_SCALAR_BOOL;
    case CLASS_FLOAT:
        return scalar == TB_SHADER_SCALAR_HALF || scalar == TB_SHADER_SCALAR_FLOAT ||
               scalar == TB_SHADER_SCALAR_MIN16FLOAT || scalar == TB_SHADER_SCALAR_LITERAL_FLOAT;
    case CLASS_FDOUBLE:
        return is_in_class(CLASS_FLOAT, scalar) || scalar == TB_SHADER_SCALAR_DOUBLE;
    case CLASS_DOUBLE:
        return scalar == TB_SHADER_SCALAR_DOUBLE;
    case CLASS_INT:
        return tb_shader_scalar_is_integer(scalar);
    case CLASS_UINT:
        return scalar == TB_SHADER_SCALAR_UINT || scalar == TB_SHADER_SCALAR_UINT16 ||
               scalar == TB_SHADER_SCALAR_UINT64 || scalar == TB_SHADER_SCALAR_MIN16UINT;
    case CLASS_ATOMIC:
        return scalar == TB_SHADER_SCALAR_INT || scalar == TB_SHADER_SCALAR_UINT ||
               scalar == TB_SHADER_SCALAR_INT64 || scalar == TB_SHADER_SCALAR_UINT64;
    case CLASS_B32:
        return scalar == TB_SHADER_SCALAR_INT || scalar == TB_SHADER_SCALAR_UINT ||
               scalar == TB_SHADER_SCALAR_FLOAT || tb_shader_scalar_is_literal(scalar);
    case CLASS_B16:
        return scalar == TB_SHADER_SCALAR_HALF || scalar == TB_SHADER_SCALAR_INT16 ||
               scalar == TB_SHADER_SCALAR_UINT16;
    }
    return false;
}

/* What a component outside a class becomes, as DXC converts it; false for
 * a class that takes nothing else. */
static bool class_fallback(component_class class_of, tb_shader_scalar *scalar)
{
    switch (class_of)
    {
    case CLASS_NUMERIC:
    case CLASS_FLOAT:
    case CLASS_FDOUBLE:
        *scalar = TB_SHADER_SCALAR_FLOAT;
        return true;
    case CLASS_DOUBLE:
        *scalar = TB_SHADER_SCALAR_DOUBLE;
        return true;
    case CLASS_INT:
    case CLASS_UINT:
        *scalar = TB_SHADER_SCALAR_UINT;
        return true;
    case CLASS_NONE:
    case CLASS_ANY:
    case CLASS_ATOMIC:
    case CLASS_B32:
    case CLASS_B16:
        return false;
    }
    return false;
}

static void declare_templates(bindings *out, const term *declared)
{
    if (declared->component == COMPONENT_TEMPLATE && declared->class_of != CLASS_NONE)
    {
        out->classes[declared->template_index] = declared->class_of;
    }
    if (declared->shape == SHAPE_TEMPLATE && declared->layout != '\0')
    {
        out->layouts[declared->shape_index] = declared->layout;
    }
}

static void bind_component(bindings *out, size_t index, tb_shader_scalar scalar)
{
    if (!out->has_component[index])
    {
        out->has_component[index] = true;
        out->components[index] = scalar;
        return;
    }
    if (out->components[index] != scalar)
    {
        out->components[index] = tb_shader_common_scalar(out->components[index], scalar);
    }
}

static uint32_t smaller(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

/* A shape combines with a scalar as the shape, and with one of its kind as
 * the smaller of the two; a vector does not combine with a matrix. */
static bool bind_shape(shape_value *shape, const tb_shader_type *type)
{
    const shape_value given = {true, type->type_class, type->rows, type->columns};
    if (!shape->is_set || shape->type_class == TB_SHADER_CLASS_SCALAR)
    {
        *shape = given;
        return true;
    }
    if (given.type_class == TB_SHADER_CLASS_SCALAR)
    {
        return true;
    }
    if (given.type_class != shape->type_class)
    {
        return false;
    }
    shape->rows = smaller(shape->rows, given.rows);
    shape->columns = smaller(shape->columns, given.columns);
    return true;
}

static void bind_size(bindings *out, size_term size, uint32_t value)
{
    if (size.letter < 0)
    {
        return;
    }
    uint32_t *const bound = &out->sizes[size.letter];
    *bound = *bound == 0 ? value : smaller(*bound, value);
}

/* A vector parameter takes a scalar or a vector; a matrix parameter, a
 * scalar or a matrix. */
static bool bind_sizes(bindings *out, const term *parameter, const tb_shader_type *type)
{
    if (parameter->shape == SHAPE_VECTOR)
    {
        if (type->type_class == TB_SHADER_CLASS_MATRIX)
        {
            return false;
        }
        bind_size(out, parameter->columns, type->columns);
        return true;
    }
    if (type->type_class == TB_SHADER_CLASS_VECTOR)
    {
        return false;
    }
    bind_size(out, parameter->rows, type->rows);
    bind_size(out, parameter->columns, type->columns);
    return true;
}

static bool bind_argument(tb_shader_checker *checker, bindings *out, const term *parameter,
                          const tb_shader_type *argument)
{
    if (parameter->component == COMPONENT_STRUCT_TEMPLATE)
    {
        const tb_shader_type **const bound = &out->structs[parameter->template_index];
        if (argument->type_class != TB_SHADER_CLASS_STRUCT ||
            (*bound != NULL && *bound != argument))
        {
            return false;
        }
        *bound = argument;
        return true;
    }
    const bool is_generic =
        parameter->component == COMPONENT_TEMPLATE || parameter->shape != SHAPE_SCALAR;
    if (parameter->component == COMPONENT_TYPE || !is_generic)
    {
        return true;
    }
    const tb_shader_type *const view = tb_shader_check_arithmetic_view(checker, argument);
    if (!tb_shader_type_is_numeric(view))
    {
        return false;
    }
    if (parameter->component == COMPONENT_TEMPLATE)
    {
        bind_component(out, parameter->template_index, view->scalar);
    }
    switch (parameter->shape)
    {
    case SHAPE_SCALAR:
        /* A template written as a scalar takes only one, as in DXC: `refract`'s
         * ratio cannot be a vector. */
        return tb_shader_component_count(view) == 1;
    case SHAPE_TEMPLATE:
        return bind_shape(&out->shapes[parameter->shape_index], view);
    case SHAPE_VECTOR:
    case SHAPE_MATRIX:
        return bind_sizes(out, parameter, view);
    }
    return false;
}

/* Settle each template's component in its class, and check each shape's layout. */
static bool settle(bindings *out)
{
    for (size_t index = 0; index < MAX_TEMPLATES; index++)
    {
        const component_class class_of = out->classes[index];
        if (class_of != CLASS_NONE && !out->has_component[index])
        {
            return false;
        }
        if (class_of != CLASS_NONE && !is_in_class(class_of, out->components[index]) &&
            !class_fallback(class_of, &out->components[index]))
        {
            return false;
        }
        const shape_value *const shape = &out->shapes[index];
        if (out->layouts[index] != '\0' && !shape->is_set)
        {
            return false;
        }
        const bool wrong_layout =
            (out->layouts[index] == 'v' && shape->type_class == TB_SHADER_CLASS_MATRIX) ||
            (out->layouts[index] == 'm' && shape->type_class == TB_SHADER_CLASS_VECTOR);
        if (wrong_layout)
        {
            return false;
        }
    }
    return true;
}

static uint32_t size_of(const bindings *in, size_term size)
{
    return size.letter < 0 ? size.value : in->sizes[size.letter];
}

/* The type a term names once its templates are decided; NULL when one is not. */
static const tb_shader_type *instantiate(tb_shader_checker *checker, const bindings *in,
                                         const term *written)
{
    tb_shader_types *const types = checker->types;
    tb_shader_scalar scalar = written->scalar;
    switch (written->component)
    {
    case COMPONENT_TYPE:
        return written->type;
    case COMPONENT_STRUCT_TEMPLATE:
        return in->structs[written->template_index];
    case COMPONENT_TEMPLATE:
        if (!in->has_component[written->template_index])
        {
            return NULL;
        }
        scalar = in->components[written->template_index];
        break;
    case COMPONENT_SCALAR:
        break;
    }
    switch (written->shape)
    {
    case SHAPE_SCALAR:
        return tb_shader_scalar_type(types, scalar);
    case SHAPE_TEMPLATE: {
        const shape_value *const shape = &in->shapes[written->shape_index];
        if (!shape->is_set)
        {
            return NULL;
        }
        if (shape->type_class == TB_SHADER_CLASS_MATRIX)
        {
            return tb_shader_matrix_type(types, scalar, shape->rows, shape->columns);
        }
        if (shape->type_class == TB_SHADER_CLASS_VECTOR)
        {
            return tb_shader_vector_type(types, scalar, shape->columns);
        }
        return tb_shader_scalar_type(types, scalar);
    }
    case SHAPE_VECTOR: {
        const uint32_t columns = size_of(in, written->columns);
        return columns == 0 ? NULL : tb_shader_vector_type(types, scalar, columns);
    }
    case SHAPE_MATRIX: {
        const uint32_t rows = size_of(in, written->rows);
        const uint32_t columns = size_of(in, written->columns);
        return rows == 0 || columns == 0 ? NULL
                                         : tb_shader_matrix_type(types, scalar, rows, columns);
    }
    }
    return NULL;
}

/* The signature one of an intrinsic's overloads has for these arguments, or
 * false when they cannot decide its templates. */
static bool intrinsic_signature(tb_shader_checker *checker, const parsed_signature *parsed,
                                tb_shader_node *const *arguments, size_t count, signature *out)
{
    if (count < parsed->count || (count > parsed->count && !parsed->is_variadic))
    {
        return false;
    }
    bindings bound = {0};
    declare_templates(&bound, &parsed->result);
    for (size_t index = 0; index < parsed->count; index++)
    {
        declare_templates(&bound, &parsed->parameters[index]);
    }
    for (size_t index = 0; index < parsed->count; index++)
    {
        const term *const parameter = &parsed->parameters[index];
        if (parameter->direction == DIRECTION_OUT)
        {
            continue;
        }
        if (!bind_argument(checker, &bound, parameter, type_of(checker, arguments[index])))
        {
            return false;
        }
    }
    if (!settle(&bound))
    {
        return false;
    }
    out->result = instantiate(checker, &bound, &parsed->result);
    out->count = parsed->count;
    out->required = parsed->count;
    out->is_variadic = parsed->is_variadic;
    for (size_t index = 0; index < parsed->count; index++)
    {
        out->parameters[index] = instantiate(checker, &bound, &parsed->parameters[index]);
        out->is_output[index] = parsed->parameters[index].direction != DIRECTION_IN;
        if (out->parameters[index] == NULL)
        {
            return false;
        }
    }
    return out->result != NULL;
}

/* -------------------------------------------------------------------------
 * Choosing an overload.
 * ------------------------------------------------------------------------- */

/* What a call names: a function of the shader's, or an intrinsic. */
typedef struct callee
{
    tb_shader_symbol *function; /* The first of its overloads. */
    const tb_shader_intrinsic_info *intrinsic;
    tb_shader_name name;
} callee;

/* One overload a call could mean. */
typedef struct candidate
{
    tb_shader_symbol *function; /* NULL for an intrinsic's. */
    size_t overload;            /* An intrinsic's: which of its signatures. */
    int costs[MAX_ARGUMENTS];
} candidate;

static bool function_signature(tb_shader_checker *checker, const tb_shader_symbol *function,
                               signature *out)
{
    const tb_shader_node *const declaration = function->declaration;
    if (declaration->parameter_count > MAX_ARGUMENTS)
    {
        return false;
    }
    out->result = function->type;
    out->count = declaration->parameter_count;
    out->required = 0;
    out->is_variadic = false;
    size_t index = 0;
    for (const tb_shader_node *parameter = declaration->parameters; parameter != NULL;
         parameter = parameter->next, index++)
    {
        out->parameters[index] = type_of(checker, parameter);
        out->is_output[index] =
            (parameter->modifiers & (TB_SHADER_MODIFIER_OUT | TB_SHADER_MODIFIER_INOUT)) != 0;
        if (parameter->value == NULL)
        {
            out->required = index + 1;
        }
    }
    return true;
}

/* The costs of passing the arguments to a signature, or false when they cannot be. */
static bool signature_costs(tb_shader_checker *checker, const signature *taken,
                            tb_shader_node *const *arguments, size_t count, candidate *out)
{
    if (count < taken->required || (count > taken->count && !taken->is_variadic))
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (index >= taken->count)
        {
            out->costs[index] = 0;
            continue;
        }
        const int cost = tb_shader_check_argument_cost(checker, type_of(checker, arguments[index]),
                                                       taken->parameters[index]);
        if (cost < 0)
        {
            return false;
        }
        out->costs[index] = cost;
    }
    return true;
}

/* Whether `a` is a better choice than `b`: no worse for any argument, and
 * better for one. */
static bool is_better(const candidate *a, const candidate *b, size_t count)
{
    bool better = false;
    for (size_t index = 0; index < count; index++)
    {
        if (a->costs[index] > b->costs[index])
        {
            return false;
        }
        better = better || a->costs[index] < b->costs[index];
    }
    return better;
}

static const char *argument_list(tb_shader_checker *checker, tb_shader_node *const *arguments,
                                 size_t count)
{
    tb_array *const text = TB_ARRAY_CREATE(&checker->allocator, char);
    for (size_t index = 0; index < count; index++)
    {
        const char *const name = type_of(checker, arguments[index])->name;
        if (index > 0)
        {
            tb_array_append(text, ", ", 2);
        }
        tb_array_append(text, name, tb_str_length(name));
    }
    const char nul = '\0';
    (void)tb_array_push(text, &nul);
    return tb_array_data(text);
}

/* The signature of an intrinsic's overload for these arguments. */
static bool overload_signature(tb_shader_checker *checker, const tb_shader_intrinsic_info *info,
                               size_t overload, tb_shader_node *const *arguments, size_t count,
                               signature *out)
{
    reader in = {checker->types, info->signatures, 0};
    parsed_signature parsed = {0};
    for (size_t index = 0; index <= overload; index++)
    {
        const bool is_valid = read_signature(&in, &parsed);
        TB_ASSERT(is_valid);
        if (!is_valid)
        {
            return false;
        }
    }
    return intrinsic_signature(checker, &parsed, arguments, count, out);
}

/* Every overload the arguments can be passed to. */
static void gather_candidates(tb_shader_checker *checker, const callee *called,
                              tb_shader_node *const *arguments, size_t count, tb_array *candidates)
{
    candidate option = {0};
    signature taken = {0};
    for (tb_shader_symbol *function = called->function; function != NULL;
         function = function->next_overload)
    {
        option.function = function;
        if (function_signature(checker, function, &taken) &&
            signature_costs(checker, &taken, arguments, count, &option))
        {
            (void)tb_array_push(candidates, &option);
        }
    }
    if (called->intrinsic == NULL)
    {
        return;
    }
    option.function = NULL;
    const char *const signatures = called->intrinsic->signatures;
    size_t overloads = 1;
    for (size_t at = 0; signatures[at] != '\0'; at++)
    {
        overloads += signatures[at] == ';' ? 1U : 0U;
    }
    for (size_t overload = 0; overload < overloads; overload++)
    {
        option.overload = overload;
        if (overload_signature(checker, called->intrinsic, overload, arguments, count, &taken) &&
            signature_costs(checker, &taken, arguments, count, &option))
        {
            (void)tb_array_push(candidates, &option);
        }
    }
}

/* The overload a call chooses, or NULL after reporting why none. */
static const candidate *choose_overload(tb_shader_checker *checker, tb_shader_node *call,
                                        const callee *called, tb_shader_node *const *arguments,
                                        size_t count)
{
    tb_array *const candidates = TB_ARRAY_CREATE(&checker->allocator, candidate);
    gather_candidates(checker, called, arguments, count, candidates);
    const size_t viable_count = tb_array_length(candidates);
    const candidate *const options = tb_array_data(candidates);
    const int length = name_length(called->name);
    const tb_shader_location at = call->left != NULL ? call->left->location : call->location;
    if (viable_count == 0)
    {
        tb_shader_check_error(checker, at, "no '%.*s' takes (%s)", length, called->name.text,
                              argument_list(checker, arguments, count));
        return NULL;
    }
    for (size_t index = 0; index < viable_count; index++)
    {
        bool best = true;
        for (size_t other = 0; other < viable_count && best; other++)
        {
            best = other == index || is_better(&options[index], &options[other], count);
        }
        if (best)
        {
            return &options[index];
        }
    }
    tb_shader_check_error(checker, at, "the call to '%.*s' with (%s) is ambiguous", length,
                          called->name.text, argument_list(checker, arguments, count));
    return NULL;
}

/* Convert each argument to its parameter's type; an output must be a place
 * the parameter's value can be written back to. */
static void bind_arguments(tb_shader_checker *checker, tb_shader_node *call, const signature *taken,
                           tb_shader_name name)
{
    size_t index = 0;
    for (tb_shader_node **link = &call->children; *link != NULL && index < taken->count;
         link = &(*link)->next, index++)
    {
        const tb_shader_type *const type = taken->parameters[index];
        if (!taken->is_output[index])
        {
            *link = tb_shader_check_convert(checker, *link, type);
            continue;
        }
        if (!(*link)->is_assignable)
        {
            tb_shader_check_error(checker, (*link)->location,
                                  "argument %zu of '%.*s' is an output, so it must be assignable",
                                  index + 1, name_length(name), name.text);
        }
        else if (!tb_shader_check_converts(checker, type, type_of(checker, *link)))
        {
            tb_shader_check_error(
                checker, (*link)->location,
                "argument %zu of '%.*s' writes a '%s' back, which cannot become '%s'", index + 1,
                name_length(name), name.text, type->name, type_of(checker, *link)->name);
        }
    }
}

static void resolve_call(tb_shader_checker *checker, tb_shader_node *call, const callee *called)
{
    tb_shader_node *arguments[MAX_ARGUMENTS] = {0};
    size_t count = 0;
    for (tb_shader_node *argument = call->children; argument != NULL; argument = argument->next)
    {
        if (count == MAX_ARGUMENTS)
        {
            tb_shader_check_error(checker, call->location, "a call takes at most %d arguments",
                                  MAX_ARGUMENTS);
            return;
        }
        if (is_error(type_of(checker, argument)))
        {
            return;
        }
        arguments[count++] = argument;
    }
    const candidate *const chosen = choose_overload(checker, call, called, arguments, count);
    if (chosen == NULL)
    {
        return;
    }
    signature taken = {0};
    bool found = false;
    if (chosen->function != NULL)
    {
        found = function_signature(checker, chosen->function, &taken);
        call->symbol = chosen->function;
    }
    else if (called->intrinsic != NULL)
    {
        found = overload_signature(checker, called->intrinsic, chosen->overload, arguments, count,
                                   &taken);
        call->intrinsic = (uint32_t)called->intrinsic->intrinsic;
    }
    TB_ASSERT(found);
    if (!found)
    {
        return;
    }
    call->checked = taken.result;
    bind_arguments(checker, call, &taken, called->name);
}

/* -------------------------------------------------------------------------
 * What is called.
 * ------------------------------------------------------------------------- */

static const tb_shader_intrinsic_info *find_intrinsic(tb_shader_checker *checker,
                                                      tb_shader_name name)
{
    if (checker->intrinsics == NULL)
    {
        const size_t count = (size_t)TB_SHADER_INTRINSIC_COUNT;
        checker->intrinsics = tb_table_create(&checker->allocator, count * 2, tb_shader_name_hash,
                                              tb_shader_name_equal);
        tb_shader_name *const names = TB_ARENA_NEW_ARRAY(checker->arena, tb_shader_name, count);
        tb_shader_intrinsic *const values =
            TB_ARENA_NEW_ARRAY(checker->arena, tb_shader_intrinsic, count);
        for (size_t index = 1; index < count; index++)
        {
            values[index] = (tb_shader_intrinsic)index;
            names[index] = tb_shader_name_of(tb_shader_intrinsic_info_of(values[index])->name);
            (void)tb_table_put(checker->intrinsics, &names[index], &values[index]);
        }
    }
    const tb_shader_intrinsic *const found = tb_table_get(checker->intrinsics, &name);
    return found == NULL ? NULL : tb_shader_intrinsic_info_of(*found);
}

/* What a named callee calls: a function of the shader's, which hides an
 * intrinsic of its name, or an intrinsic. */
static bool named_callee(tb_shader_checker *checker, tb_shader_node *name, callee *out)
{
    tb_shader_symbol *const symbol = tb_shader_check_lookup(checker, name->name);
    if (symbol == NULL)
    {
        out->intrinsic = find_intrinsic(checker, name->name);
        if (out->intrinsic == NULL)
        {
            tb_shader_check_error(checker, name->location, "'%.*s' is not declared",
                                  name_length(name->name), name->name.text);
        }
        return out->intrinsic != NULL;
    }
    if (symbol->kind != TB_SHADER_SYMBOL_FUNCTION)
    {
        tb_shader_check_error(checker, name->location, "'%.*s' is not a function",
                              name_length(name->name), name->name.text);
        return false;
    }
    name->symbol = symbol;
    out->function = symbol;
    return true;
}

/* What a callee calls: a function, an intrinsic, or a method of a struct. */
static bool find_callee(tb_shader_checker *checker, tb_shader_node *node, callee *out)
{
    if (node == NULL)
    {
        return false;
    }
    out->name = node->name;
    if (node->kind == TB_SHADER_NODE_NAME)
    {
        return named_callee(checker, node, out);
    }
    if (node->kind != TB_SHADER_NODE_MEMBER)
    {
        tb_shader_check_error(checker, node->location, "this expression cannot be called");
        return false;
    }
    node->left = tb_shader_check_expression(checker, node->left);
    const tb_shader_type *const object = type_of(checker, node->left);
    if (is_error(object))
    {
        return false;
    }
    tb_shader_symbol *const method = object->type_class == TB_SHADER_CLASS_STRUCT
                                         ? tb_shader_find_member(object, node->name)
                                         : NULL;
    if (method == NULL || method->kind != TB_SHADER_SYMBOL_FUNCTION)
    {
        tb_shader_check_error(checker, node->location, "'%s' has no method '%.*s'", object->name,
                              name_length(node->name), node->name.text);
        return false;
    }
    node->symbol = method;
    out->function = method;
    return true;
}

void tb_shader_check_call(tb_shader_checker *checker, tb_shader_node *node)
{
    node->checked = tb_shader_error_type(checker->types);
    callee called = {0};
    const bool found = find_callee(checker, node->left, &called);
    tb_shader_check_list(checker, &node->children);
    if (node->left != NULL && node->left->template_arguments != NULL)
    {
        tb_shader_check_error(checker, node->location, "templates are not supported yet");
        return;
    }
    if (found)
    {
        resolve_call(checker, node, &called);
    }
}
