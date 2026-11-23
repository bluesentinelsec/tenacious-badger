/* Checking declarations: the types they name, structs and their members,
 * enums, typedefs, buffers, namespaces, variables, and functions and their
 * overloads. */
#include "shader_check_internal.h"

#include "badger/str.h"

#include <string.h>

/* The most components a texture's or a typed buffer's element may have. */
#define MAX_ELEMENT_COMPONENTS 4u
/* The most control points a patch may have. */
#define MAX_PATCH_POINTS 32
/* The most samples a multisampled texture may have. */
#define MAX_SAMPLES 128

static void check_declaration(tb_shader_checker *checker, tb_shader_node *node);

static bool is_name(tb_shader_name name, const char *text)
{
    const tb_shader_name other = tb_shader_name_of(text);
    return tb_shader_name_equal(&name, &other);
}

static int name_length(tb_shader_name name)
{
    return (int)name.length;
}

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

/* A template argument that must be a type. */
static const tb_shader_type *argument_type(tb_shader_checker *checker,
                                           const tb_shader_template_argument *argument,
                                           tb_shader_location location)
{
    if (argument->type == NULL)
    {
        tb_shader_check_error(checker, location, "expected a type as a template argument");
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_check_type(checker, argument->type);
}

/* A template argument that must be a constant integer in [minimum, maximum]. */
static bool argument_count(tb_shader_checker *checker, const tb_shader_template_argument *argument,
                           tb_shader_location location, int64_t minimum, int64_t maximum,
                           uint32_t *count)
{
    if (argument->value == NULL)
    {
        tb_shader_check_error(checker, location, "expected a constant as a template argument");
        return false;
    }
    int64_t value = 0;
    if (!tb_shader_check_constant_integer(checker, argument->value, &value))
    {
        return false;
    }
    if (value < minimum || value > maximum)
    {
        tb_shader_check_error(checker, argument->value->location,
                              "the template argument must be from %lld to %lld, not %lld",
                              (long long)minimum, (long long)maximum, (long long)value);
        return false;
    }
    *count = (uint32_t)value;
    return true;
}

static bool expect_arguments(tb_shader_checker *checker, const tb_shader_type_syntax *syntax,
                             size_t minimum, size_t maximum)
{
    if (syntax->argument_count >= minimum && syntax->argument_count <= maximum)
    {
        return true;
    }
    if (maximum == 0)
    {
        tb_shader_check_error(checker, syntax->location, "'%.*s' takes no template arguments",
                              name_length(syntax->name), syntax->name.text);
    }
    else if (minimum == maximum)
    {
        tb_shader_check_error(checker, syntax->location,
                              "'%.*s' takes %zu template argument%s, not %zu",
                              name_length(syntax->name), syntax->name.text, minimum,
                              minimum == 1 ? "" : "s", syntax->argument_count);
    }
    else
    {
        tb_shader_check_error(
            checker, syntax->location, "'%.*s' takes from %zu to %zu template arguments, not %zu",
            name_length(syntax->name), syntax->name.text, minimum, maximum, syntax->argument_count);
    }
    return false;
}

/* `vector`, `vector<float, 3>`, `matrix`, `matrix<half, 2, 3>`. */
static const tb_shader_type *generic_numeric(tb_shader_checker *checker,
                                             const tb_shader_type_syntax *syntax, bool is_matrix)
{
    if (syntax->argument_count == 0)
    {
        return is_matrix ? tb_shader_matrix_type(checker->types, TB_SHADER_SCALAR_FLOAT, 4, 4)
                         : tb_shader_vector_type(checker->types, TB_SHADER_SCALAR_FLOAT, 4);
    }
    const size_t wanted = is_matrix ? 3 : 2;
    if (!expect_arguments(checker, syntax, wanted, wanted))
    {
        return tb_shader_error_type(checker->types);
    }
    const tb_shader_template_argument *const first = syntax->arguments;
    const tb_shader_type *const component = argument_type(checker, first, syntax->location);
    if (component->type_class == TB_SHADER_CLASS_ERROR)
    {
        return component;
    }
    if (component->type_class != TB_SHADER_CLASS_SCALAR)
    {
        tb_shader_check_error(checker, syntax->location,
                              "the components of a vector or matrix must be scalars, not '%s'",
                              component->name);
        return tb_shader_error_type(checker->types);
    }
    uint32_t rows = 1;
    uint32_t columns = 1;
    const tb_shader_template_argument *const second = first->next;
    if (!argument_count(checker, second, syntax->location, 1, 4, is_matrix ? &rows : &columns))
    {
        return tb_shader_error_type(checker->types);
    }
    if (is_matrix && !argument_count(checker, second->next, syntax->location, 1, 4, &columns))
    {
        return tb_shader_error_type(checker->types);
    }
    return is_matrix ? tb_shader_matrix_type(checker->types, component->scalar, rows, columns)
                     : tb_shader_vector_type(checker->types, component->scalar, columns);
}

/* The element of a texture or a typed buffer: a scalar or a short vector. */
static bool check_texel(tb_shader_checker *checker, const tb_shader_type *element,
                        const tb_shader_object_info *info, tb_shader_location location)
{
    const bool is_texel = (element->type_class == TB_SHADER_CLASS_SCALAR ||
                           element->type_class == TB_SHADER_CLASS_VECTOR) &&
                          element->columns <= MAX_ELEMENT_COMPONENTS;
    if (is_texel || element->type_class == TB_SHADER_CLASS_ERROR)
    {
        return true;
    }
    tb_shader_check_error(checker, location,
                          "the element of '%s' must be a scalar or a vector, not '%s'", info->name,
                          element->name);
    return false;
}

static const tb_shader_type *element_object(tb_shader_checker *checker,
                                            const tb_shader_type_syntax *syntax,
                                            const tb_shader_object_info *info)
{
    if (!expect_arguments(checker, syntax, 0, 1))
    {
        return tb_shader_error_type(checker->types);
    }
    const tb_shader_type *element =
        tb_shader_vector_type(checker->types, TB_SHADER_SCALAR_FLOAT, 4);
    if (syntax->argument_count == 1)
    {
        element = argument_type(checker, syntax->arguments, syntax->location);
    }
    if (!check_texel(checker, element, info, syntax->location))
    {
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_object_type(checker->types, info->object, element, 0, 0);
}

static const tb_shader_type *structure_object(tb_shader_checker *checker,
                                              const tb_shader_type_syntax *syntax,
                                              const tb_shader_object_info *info)
{
    if (!expect_arguments(checker, syntax, 1, 1))
    {
        return tb_shader_error_type(checker->types);
    }
    const tb_shader_type *const element =
        argument_type(checker, syntax->arguments, syntax->location);
    if (element->type_class == TB_SHADER_CLASS_ERROR)
    {
        return element;
    }
    const bool needs_struct = info->object == TB_SHADER_OBJECT_CONSTANT_BUFFER ||
                              info->object == TB_SHADER_OBJECT_TEXTURE_BUFFER;
    if (needs_struct && element->type_class != TB_SHADER_CLASS_STRUCT)
    {
        tb_shader_check_error(checker, syntax->location,
                              "the element of '%s' must be a struct, not '%s'", info->name,
                              element->name);
        return tb_shader_error_type(checker->types);
    }
    if (tb_shader_flat_count(element) == 0)
    {
        tb_shader_check_error(checker, syntax->location, "'%s' cannot hold '%s'", info->name,
                              element->name);
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_object_type(checker->types, info->object, element, 0, 0);
}

static const tb_shader_type *counted_object(tb_shader_checker *checker,
                                            const tb_shader_type_syntax *syntax,
                                            const tb_shader_object_info *info)
{
    const bool is_patch = info->template_kind == TB_SHADER_TEMPLATE_PATCH;
    if (!expect_arguments(checker, syntax, is_patch ? 2 : 1, 2))
    {
        return tb_shader_error_type(checker->types);
    }
    const tb_shader_type *const element =
        argument_type(checker, syntax->arguments, syntax->location);
    if (element->type_class == TB_SHADER_CLASS_ERROR)
    {
        return element;
    }
    if (!is_patch && !check_texel(checker, element, info, syntax->location))
    {
        return tb_shader_error_type(checker->types);
    }
    uint32_t count = 0;
    const int64_t maximum = is_patch ? MAX_PATCH_POINTS : MAX_SAMPLES;
    if (syntax->argument_count == 2 &&
        !argument_count(checker, syntax->arguments->next, syntax->location, 1, maximum, &count))
    {
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_object_type(checker->types, info->object, element, count, 0);
}

static const tb_shader_type *flags_object(tb_shader_checker *checker,
                                          const tb_shader_type_syntax *syntax,
                                          const tb_shader_object_info *info)
{
    if (!expect_arguments(checker, syntax, 1, 2))
    {
        return tb_shader_error_type(checker->types);
    }
    uint32_t flags = 0;
    uint32_t second = 0;
    if (!argument_count(checker, syntax->arguments, syntax->location, 0, UINT32_MAX, &flags))
    {
        return tb_shader_error_type(checker->types);
    }
    if (syntax->argument_count == 2 &&
        !argument_count(checker, syntax->arguments->next, syntax->location, 0, UINT32_MAX, &second))
    {
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_object_type(checker->types, info->object, NULL, flags, second);
}

static const tb_shader_type *object_type(tb_shader_checker *checker,
                                         const tb_shader_type_syntax *syntax,
                                         const tb_shader_object_info *info)
{
    switch (info->template_kind)
    {
    case TB_SHADER_TEMPLATE_NONE:
        if (!expect_arguments(checker, syntax, 0, 0))
        {
            return tb_shader_error_type(checker->types);
        }
        return tb_shader_object_type(checker->types, info->object, NULL, 0, 0);
    case TB_SHADER_TEMPLATE_ELEMENT:
        return element_object(checker, syntax, info);
    case TB_SHADER_TEMPLATE_STRUCTURE:
        return structure_object(checker, syntax, info);
    case TB_SHADER_TEMPLATE_SAMPLES:
    case TB_SHADER_TEMPLATE_PATCH:
        return counted_object(checker, syntax, info);
    case TB_SHADER_TEMPLATE_FLAGS:
        return flags_object(checker, syntax, info);
    }
    return tb_shader_error_type(checker->types);
}

/* A type named by a word HLSL builds in, or NULL when the name is not one. */
static const tb_shader_type *builtin_type(tb_shader_checker *checker,
                                          const tb_shader_type_syntax *syntax)
{
    const tb_shader_name name = syntax->name;
    if (is_name(name, "vector") || is_name(name, "matrix"))
    {
        return generic_numeric(checker, syntax, is_name(name, "matrix"));
    }
    const tb_shader_object_info *const info = tb_shader_find_object(name);
    if (info != NULL)
    {
        return object_type(checker, syntax, info);
    }
    const tb_shader_type *simple = tb_shader_numeric_type(checker->types, name);
    if (is_name(name, "void"))
    {
        simple = tb_shader_void_type(checker->types);
    }
    else if (is_name(name, "string"))
    {
        simple = tb_shader_string_type(checker->types);
    }
    else if (is_name(name, "RayDesc"))
    {
        simple = tb_shader_ray_desc_type(checker->types);
    }
    else if (is_name(name, "BuiltInTriangleIntersectionAttributes"))
    {
        simple = tb_shader_triangle_attributes_type(checker->types);
    }
    else if (is_name(name, "int8_t4_packed") || is_name(name, "uint8_t4_packed"))
    {
        simple = tb_shader_scalar_type(checker->types, TB_SHADER_SCALAR_UINT);
    }
    if (simple != NULL && !expect_arguments(checker, syntax, 0, 0))
    {
        return tb_shader_error_type(checker->types);
    }
    return simple;
}

static const tb_shader_type *named_type(tb_shader_checker *checker,
                                        const tb_shader_type_syntax *syntax)
{
    if (syntax->definition != NULL)
    {
        const tb_shader_type *const defined = syntax->definition->checked;
        return defined != NULL ? defined : tb_shader_error_type(checker->types);
    }
    const tb_shader_symbol *const symbol = tb_shader_check_lookup(checker, syntax->name);
    if (symbol != NULL && symbol->kind == TB_SHADER_SYMBOL_TYPE)
    {
        if (!expect_arguments(checker, syntax, 0, 0))
        {
            return tb_shader_error_type(checker->types);
        }
        return symbol->type;
    }
    const tb_shader_type *const builtin = builtin_type(checker, syntax);
    if (builtin != NULL)
    {
        return builtin;
    }
    tb_shader_check_error(checker, syntax->location, "unknown type '%.*s'",
                          name_length(syntax->name), syntax->name.text);
    return tb_shader_error_type(checker->types);
}

static const tb_shader_type *array_type(tb_shader_checker *checker,
                                        const tb_shader_type_syntax *syntax)
{
    const tb_shader_type *const element = tb_shader_check_type(checker, syntax->element);
    if (element->type_class == TB_SHADER_CLASS_ERROR)
    {
        return element;
    }
    if (element->type_class == TB_SHADER_CLASS_VOID)
    {
        tb_shader_check_error(checker, syntax->location, "an array cannot hold 'void'");
        return tb_shader_error_type(checker->types);
    }
    if (element->type_class == TB_SHADER_CLASS_ARRAY && element->length == 0)
    {
        tb_shader_check_error(checker, syntax->location,
                              "only the first size of an array may be left out");
        return tb_shader_error_type(checker->types);
    }
    if (syntax->size == NULL)
    {
        return tb_shader_array_type(checker->types, element, 0);
    }
    int64_t length = 0;
    if (!tb_shader_check_constant_integer(checker, syntax->size, &length))
    {
        return tb_shader_error_type(checker->types);
    }
    if (length <= 0 || length > (int64_t)INT32_MAX)
    {
        tb_shader_check_error(checker, syntax->size->location,
                              "the size of an array must be from 1 to %d, not %lld", INT32_MAX,
                              (long long)length);
        return tb_shader_error_type(checker->types);
    }
    return tb_shader_array_type(checker->types, element, (uint32_t)length);
}

const tb_shader_type *tb_shader_check_type(tb_shader_checker *checker,
                                           const tb_shader_type_syntax *syntax)
{
    if (syntax == NULL)
    {
        return tb_shader_error_type(checker->types);
    }
    if (syntax->kind == TB_SHADER_TYPE_SYNTAX_ARRAY)
    {
        return array_type(checker, syntax);
    }
    return named_type(checker, syntax);
}

/* Whether a type is fully known: not a struct still being declared, nor an
 * array of one. Reports it when not. */
static bool check_complete(tb_shader_checker *checker, const tb_shader_type *type,
                           tb_shader_location location)
{
    const tb_shader_type *inner = type;
    while (inner->type_class == TB_SHADER_CLASS_ARRAY)
    {
        inner = inner->element;
    }
    if (inner->type_class == TB_SHADER_CLASS_STRUCT && !inner->structure->is_complete)
    {
        tb_shader_check_error(checker, location, "'%s' is used before its declaration is complete",
                              inner->name);
        return false;
    }
    if (type->type_class == TB_SHADER_CLASS_VOID)
    {
        tb_shader_check_error(checker, location, "a variable cannot be 'void'");
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Namespaces, typedefs and enums.
 * ------------------------------------------------------------------------- */

static tb_shader_name enter_path(tb_shader_checker *checker, tb_shader_name name)
{
    const tb_shader_name outer = checker->path;
    checker->path = outer.length == 0 ? name : tb_shader_check_join(checker, outer, name);
    return outer;
}

static void check_namespace(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_name qualified = checker->path.length == 0
                                         ? node->name
                                         : tb_shader_check_join(checker, checker->path, node->name);
    tb_shader_symbol *existing = tb_table_get(checker->globals, &qualified);
    if (existing == NULL)
    {
        existing =
            tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_NAMESPACE, node->name, node);
        (void)tb_shader_check_declare(checker, existing, node->location);
    }
    else if (existing->kind != TB_SHADER_SYMBOL_NAMESPACE)
    {
        tb_shader_check_error(checker, node->location, "redefinition of '%.*s'",
                              name_length(qualified), qualified.text);
        return;
    }
    node->symbol = existing;
    const tb_shader_name outer = enter_path(checker, node->name);
    tb_shader_check_declarations(checker, node->children);
    checker->path = outer;
}

static void check_typedef(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *const type = tb_shader_check_type(checker, node->type);
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_TYPE, node->name, node);
    symbol->type = type;
    node->checked = type;
    node->symbol = symbol;
    (void)tb_shader_check_declare(checker, symbol, node->location);
}

static void check_enumerators(tb_shader_checker *checker, tb_shader_node *node,
                              const tb_shader_type *type)
{
    int64_t next = 0;
    for (tb_shader_node *enumerator = node->children; enumerator != NULL;
         enumerator = enumerator->next)
    {
        int64_t value = next;
        if (enumerator->value != NULL &&
            !tb_shader_check_constant_integer(checker, enumerator->value, &value))
        {
            value = next;
        }
        tb_shader_symbol *const symbol = tb_shader_check_new_symbol(
            checker, TB_SHADER_SYMBOL_ENUMERATOR, enumerator->name, enumerator);
        symbol->type = type;
        symbol->is_const = true;
        symbol->has_value = true;
        symbol->value.bits = (uint64_t)value;
        enumerator->symbol = symbol;
        enumerator->checked = type;
        (void)tb_shader_check_declare(checker, symbol, enumerator->location);
        next = value == INT64_MAX ? value : value + 1;
    }
}

static void check_enum(tb_shader_checker *checker, tb_shader_node *node)
{
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_TYPE, node->name, node);
    (void)tb_shader_check_declare(checker, symbol, node->location);
    tb_shader_type *const type = tb_shader_enum_type(checker->types, symbol->name, node);
    if (node->type != NULL)
    {
        const tb_shader_type *const underlying = tb_shader_check_type(checker, node->type);
        const bool is_integer = underlying->type_class == TB_SHADER_CLASS_SCALAR &&
                                tb_shader_scalar_is_integer(underlying->scalar);
        if (is_integer)
        {
            type->scalar = underlying->scalar;
        }
        else if (underlying->type_class != TB_SHADER_CLASS_ERROR)
        {
            tb_shader_check_error(checker, node->type->location,
                                  "an enum's type must be an integer, not '%s'", underlying->name);
        }
    }
    symbol->type = type;
    node->checked = type;
    node->symbol = symbol;
    /* The enumerators of `enum class E` are `E::X`. */
    tb_shader_name outer = checker->path;
    if (node->is_scoped && checker->scope_depth == 0)
    {
        outer = enter_path(checker, node->name);
    }
    check_enumerators(checker, node, type);
    checker->path = outer;
}

/* -------------------------------------------------------------------------
 * Functions.
 * ------------------------------------------------------------------------- */

static void check_parameter(tb_shader_checker *checker, tb_shader_node *parameter)
{
    const tb_shader_type *const type = tb_shader_check_type(checker, parameter->type);
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_PARAMETER, parameter->name, parameter);
    symbol->type = type;
    symbol->modifiers = parameter->modifiers;
    symbol->storage = TB_SHADER_STORAGE_PARAMETER;
    symbol->is_const = (parameter->modifiers & TB_SHADER_MODIFIER_CONST) != 0;
    parameter->symbol = symbol;
    parameter->checked = type;
    if (type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0)
    {
        tb_shader_check_error(checker, parameter->location, "a parameter's array needs a size");
    }
    else if (type->type_class == TB_SHADER_CLASS_VOID)
    {
        tb_shader_check_error(checker, parameter->location, "a parameter cannot be 'void'");
    }
    if (parameter->value != NULL)
    {
        parameter->value = tb_shader_check_convert(
            checker, tb_shader_check_expression(checker, parameter->value), type);
    }
}

static bool is_output(const tb_shader_node *parameter)
{
    return (parameter->modifiers & (TB_SHADER_MODIFIER_OUT | TB_SHADER_MODIFIER_INOUT)) != 0;
}

/* Whether two functions take the same parameters, so one declares the other. */
static bool same_parameters(const tb_shader_node *a, const tb_shader_node *b)
{
    if (a->parameter_count != b->parameter_count)
    {
        return false;
    }
    const tb_shader_node *left = a->parameters;
    const tb_shader_node *right = b->parameters;
    for (; left != NULL && right != NULL; left = left->next, right = right->next)
    {
        if (left->checked != right->checked || is_output(left) != is_output(right))
        {
            return false;
        }
    }
    return true;
}

/* Join a function to its overloads, or to its earlier declaration. Returns
 * the symbol that is the function. */
static tb_shader_symbol *add_overload(tb_shader_checker *checker, tb_shader_symbol *first,
                                      tb_shader_node *node)
{
    const tb_shader_name name = first->name;
    const tb_shader_type *const owner = first->owner;
    tb_shader_symbol *last = first;
    tb_shader_symbol *overload = first;
    do
    {
        last = overload;
        overload = overload->next_overload;
        if (!same_parameters(last->declaration, node))
        {
            continue;
        }
        if (last->type != node->checked)
        {
            tb_shader_check_error(
                checker, node->location, "'%.*s' is declared again returning '%s', not '%s'",
                name_length(node->name), node->name.text, node->checked->name, last->type->name);
        }
        if (last->is_defined && node->body != NULL)
        {
            tb_shader_check_error(checker, node->location, "redefinition of '%.*s'",
                                  name_length(node->name), node->name.text);
        }
        else if (node->body != NULL)
        {
            last->declaration = node;
            last->is_defined = true;
        }
        return last;
    } while (overload != NULL);
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_FUNCTION, name, node);
    symbol->type = node->checked;
    symbol->owner = owner;
    symbol->is_defined = node->body != NULL;
    last->next_overload = symbol;
    return symbol;
}

/* Declare a function or a method; `members` is its struct's, or NULL. */
static tb_shader_symbol *declare_function(tb_shader_checker *checker, tb_shader_node *node,
                                          const tb_shader_type *owner)
{
    tb_shader_symbol *first = NULL;
    if (owner != NULL)
    {
        first = tb_table_get(owner->structure->members, &node->name);
    }
    else if (checker->scope_depth == 0)
    {
        const tb_shader_name qualified =
            checker->path.length == 0 ? node->name
                                      : tb_shader_check_join(checker, checker->path, node->name);
        first = tb_table_get(checker->globals, &qualified);
    }
    if (first != NULL && first->kind == TB_SHADER_SYMBOL_FUNCTION)
    {
        return add_overload(checker, first, node);
    }
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_FUNCTION, node->name, node);
    symbol->type = node->checked;
    symbol->owner = owner;
    symbol->is_defined = node->body != NULL;
    if (owner != NULL)
    {
        if (first != NULL)
        {
            tb_shader_check_error(checker, node->location, "'%.*s' is already a member of '%s'",
                                  name_length(node->name), node->name.text, owner->name);
            return symbol;
        }
        (void)tb_table_put(owner->structure->members, &symbol->name, symbol);
        return symbol;
    }
    (void)tb_shader_check_declare(checker, symbol, node->location);
    return symbol;
}

/* The return type and the parameters of a function. */
static void check_signature(tb_shader_checker *checker, tb_shader_node *node)
{
    node->checked = tb_shader_check_type(checker, node->type);
    for (tb_shader_node *parameter = node->parameters; parameter != NULL;
         parameter = parameter->next)
    {
        check_parameter(checker, parameter);
    }
}

/* `float Light::Intensity() { ... }`: a method defined outside its struct. */
static void check_method_definition(tb_shader_checker *checker, tb_shader_node *node)
{
    size_t split = node->name.length;
    while (split >= 2 && !(node->name.text[split - 2] == ':' && node->name.text[split - 1] == ':'))
    {
        split--;
    }
    const tb_shader_name owner_name = {node->name.text, split - 2};
    const tb_shader_name method = {node->name.text + split, node->name.length - split};
    const tb_shader_symbol *const owner = tb_shader_check_lookup(checker, owner_name);
    if (owner == NULL || owner->kind != TB_SHADER_SYMBOL_TYPE ||
        owner->type->type_class != TB_SHADER_CLASS_STRUCT)
    {
        tb_shader_check_error(checker, node->location, "'%.*s' is not a struct",
                              name_length(owner_name), owner_name.text);
        return;
    }
    check_signature(checker, node);
    tb_shader_symbol *const first = tb_table_get(owner->type->structure->members, &method);
    tb_shader_symbol *found = NULL;
    for (tb_shader_symbol *overload = first; overload != NULL; overload = overload->next_overload)
    {
        if (overload->kind == TB_SHADER_SYMBOL_FUNCTION &&
            same_parameters(overload->declaration, node))
        {
            found = overload;
        }
    }
    if (found == NULL)
    {
        tb_shader_check_error(checker, node->location, "'%s' declares no method '%.*s' like this",
                              owner->type->name, name_length(method), method.text);
        return;
    }
    if (found->is_defined)
    {
        tb_shader_check_error(checker, node->location, "redefinition of '%.*s'",
                              name_length(node->name), node->name.text);
        return;
    }
    found->declaration = node;
    found->is_defined = true;
    node->symbol = found;
    const tb_shader_type *const outer_owner = checker->method_owner;
    checker->method_owner = owner->type;
    tb_shader_check_body(checker, node);
    checker->method_owner = outer_owner;
}

static bool is_qualified(tb_shader_name name)
{
    return tb_str_find(name.text, 0, name.length, "::") != TB_STR_NOT_FOUND;
}

static void check_function(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node->is_template)
    {
        tb_shader_check_error(checker, node->location, "templates are not supported yet");
        return;
    }
    if (is_qualified(node->name))
    {
        check_method_definition(checker, node);
        return;
    }
    check_signature(checker, node);
    node->symbol = declare_function(checker, node, NULL);
    if (node->body != NULL)
    {
        tb_shader_check_body(checker, node);
    }
}

/* -------------------------------------------------------------------------
 * Structs.
 * ------------------------------------------------------------------------- */

static void add_field(tb_shader_checker *checker, tb_shader_structure *structure, tb_array *fields,
                      tb_shader_symbol *field, tb_shader_location location)
{
    if (tb_table_get(structure->members, &field->name) != NULL)
    {
        tb_shader_check_error(checker, location, "duplicate member '%.*s'",
                              name_length(field->name), field->name.text);
        return;
    }
    field->field_index = tb_array_length(fields);
    (void)tb_array_push(fields, &field);
    (void)tb_table_put(structure->members, &field->name, field);
}

static void inherit(tb_shader_checker *checker, tb_shader_node *node, tb_shader_type *type,
                    tb_array *fields)
{
    const tb_shader_type *const base = tb_shader_check_type(checker, node->type);
    if (base->type_class == TB_SHADER_CLASS_ERROR)
    {
        return;
    }
    if (base->type_class != TB_SHADER_CLASS_STRUCT || !base->structure->is_complete)
    {
        tb_shader_check_error(checker, node->type->location, "'%s' cannot be a base", base->name);
        return;
    }
    type->structure->base = base;
    for (size_t index = 0; index < base->structure->field_count; index++)
    {
        tb_shader_symbol *const field = base->structure->fields[index];
        (void)tb_array_push(fields, &field);
        (void)tb_table_put(type->structure->members, &field->name, field);
    }
}

static void check_bit_width(tb_shader_checker *checker, tb_shader_node *member,
                            const tb_shader_type *type)
{
    const bool is_integer =
        type->type_class == TB_SHADER_CLASS_SCALAR && tb_shader_scalar_is_integer(type->scalar);
    if (!is_integer)
    {
        if (type->type_class != TB_SHADER_CLASS_ERROR)
        {
            tb_shader_check_error(checker, member->location,
                                  "a bit field must be an integer, not '%s'", type->name);
        }
        return;
    }
    const int64_t bits =
        type->scalar == TB_SHADER_SCALAR_INT64 || type->scalar == TB_SHADER_SCALAR_UINT64 ? 64 : 32;
    int64_t width = 0;
    if (tb_shader_check_constant_integer(checker, member->value, &width) &&
        (width < 1 || width > bits))
    {
        tb_shader_check_error(checker, member->value->location,
                              "a bit field of '%s' must be from 1 to %lld bits wide, not %lld",
                              type->name, (long long)bits, (long long)width);
    }
}

static void check_field(tb_shader_checker *checker, tb_shader_type *owner, tb_array *fields,
                        tb_shader_node *member)
{
    const tb_shader_type *const type = tb_shader_check_type(checker, member->type);
    tb_shader_symbol *const field =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_FIELD, member->name, member);
    field->type = type;
    field->owner = owner;
    field->modifiers = member->modifiers;
    member->symbol = field;
    member->checked = type;
    if (type->type_class != TB_SHADER_CLASS_ERROR &&
        !check_complete(checker, type, member->location))
    {
        field->type = tb_shader_error_type(checker->types);
    }
    else if (type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0)
    {
        tb_shader_check_error(checker, member->location, "a member's array needs a size");
    }
    if ((member->modifiers & TB_SHADER_MODIFIER_STATIC) != 0)
    {
        tb_shader_check_error(checker, member->location, "static members are not supported");
    }
    if (member->is_bit_field)
    {
        check_bit_width(checker, member, type);
    }
    else if (member->value != NULL)
    {
        tb_shader_check_error(checker, member->value->location,
                              "a struct's member cannot have an initializer");
    }
    add_field(checker, owner->structure, fields, field, member->location);
}

/* The members of a struct: its fields and its methods' declarations first,
 * so a method may use any member, then its methods' bodies. */
static void check_members(tb_shader_checker *checker, tb_shader_node *node, tb_shader_type *type,
                          tb_array *fields)
{
    for (tb_shader_node *member = node->children; member != NULL && !checker->stopped;
         member = member->next)
    {
        if (member->kind == TB_SHADER_NODE_VARIABLE)
        {
            check_field(checker, type, fields, member);
        }
        else if (member->kind == TB_SHADER_NODE_FUNCTION)
        {
            if (member->is_template)
            {
                tb_shader_check_error(checker, member->location, "templates are not supported yet");
                continue;
            }
            check_signature(checker, member);
            member->symbol = declare_function(checker, member, type);
        }
        else
        {
            check_declaration(checker, member);
        }
    }
    type->structure->fields = tb_array_data(fields);
    type->structure->field_count = tb_array_length(fields);
    type->structure->is_complete = true;
    const tb_shader_type *const outer_owner = checker->method_owner;
    checker->method_owner = type;
    for (tb_shader_node *member = node->children; member != NULL && !checker->stopped;
         member = member->next)
    {
        if (member->kind == TB_SHADER_NODE_FUNCTION && member->body != NULL &&
            member->symbol != NULL)
        {
            tb_shader_check_body(checker, member);
        }
    }
    checker->method_owner = outer_owner;
}

static void check_struct(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node->is_template)
    {
        tb_shader_check_error(checker, node->location, "templates are not supported yet");
        return;
    }
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_TYPE, node->name, node);
    (void)tb_shader_check_declare(checker, symbol, node->location);
    tb_shader_type *const type = tb_shader_struct_type(checker->types, symbol->name, node);
    symbol->type = type;
    node->symbol = symbol;
    node->checked = type;
    tb_array *const fields = TB_ARRAY_CREATE(&checker->allocator, tb_shader_symbol *);
    if (node->type != NULL)
    {
        inherit(checker, node, type, fields);
    }
    /* Types declared in a struct are `S::T`. */
    tb_shader_name outer = checker->path;
    const bool enters = checker->scope_depth == 0 && node->name.length > 0;
    if (enters)
    {
        outer = enter_path(checker, node->name);
    }
    check_members(checker, node, type, fields);
    checker->path = outer;
}

/* -------------------------------------------------------------------------
 * Variables and buffers.
 * ------------------------------------------------------------------------- */

static const uint64_t storage_modifiers = TB_SHADER_MODIFIER_STATIC |
                                          TB_SHADER_MODIFIER_GROUPSHARED |
                                          TB_SHADER_MODIFIER_UNIFORM | TB_SHADER_MODIFIER_EXTERN;

/* Where a variable lives, from where it is declared and its modifiers. */
static tb_shader_storage variable_storage(tb_shader_checker *checker, const tb_shader_node *node,
                                          bool in_buffer)
{
    const uint64_t modifiers = node->modifiers;
    const bool is_static = (modifiers & TB_SHADER_MODIFIER_STATIC) != 0;
    const bool is_groupshared = (modifiers & TB_SHADER_MODIFIER_GROUPSHARED) != 0;
    if (in_buffer)
    {
        if ((modifiers & storage_modifiers & ~TB_SHADER_MODIFIER_UNIFORM) != 0)
        {
            tb_shader_check_error(checker, node->location,
                                  "a buffer's member cannot be static, groupshared or extern");
        }
        return TB_SHADER_STORAGE_UNIFORM;
    }
    if (is_static && is_groupshared)
    {
        tb_shader_check_error(checker, node->location,
                              "a variable cannot be both static and groupshared");
    }
    if (checker->scope_depth > 0)
    {
        if ((modifiers & (storage_modifiers & ~TB_SHADER_MODIFIER_STATIC)) != 0)
        {
            tb_shader_check_error(checker, node->location,
                                  "only 'static' may set where a local variable lives");
        }
        return is_static ? TB_SHADER_STORAGE_STATIC : TB_SHADER_STORAGE_LOCAL;
    }
    if (is_static)
    {
        return TB_SHADER_STORAGE_STATIC;
    }
    return is_groupshared ? TB_SHADER_STORAGE_GROUPSHARED : TB_SHADER_STORAGE_UNIFORM;
}

static void check_variable_in(tb_shader_checker *checker, tb_shader_node *node,
                              tb_shader_node *buffer)
{
    const tb_shader_type *type = tb_shader_check_type(checker, node->type);
    tb_shader_symbol *const symbol =
        tb_shader_check_new_symbol(checker, TB_SHADER_SYMBOL_VARIABLE, node->name, node);
    symbol->modifiers = node->modifiers;
    symbol->buffer = buffer;
    symbol->storage = variable_storage(checker, node, buffer != NULL);
    /* A global that is not static is a constant of the shader's inputs. */
    symbol->is_const = (node->modifiers & TB_SHADER_MODIFIER_CONST) != 0 ||
                       symbol->storage == TB_SHADER_STORAGE_UNIFORM;
    if (type->type_class != TB_SHADER_CLASS_ERROR && !check_complete(checker, type, node->location))
    {
        type = tb_shader_error_type(checker->types);
    }
    if (node->value != NULL)
    {
        uint32_t length = 0;
        node->value = tb_shader_check_initializer(checker, node->value, type, &length);
        if (type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0 && length > 0)
        {
            type = tb_shader_array_type(checker->types, type->element, length);
        }
    }
    const bool is_unbounded_resource = symbol->storage == TB_SHADER_STORAGE_UNIFORM &&
                                       type->type_class == TB_SHADER_CLASS_ARRAY &&
                                       type->element->type_class == TB_SHADER_CLASS_OBJECT;
    if (type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0 && !is_unbounded_resource)
    {
        tb_shader_check_error(checker, node->location,
                              "the array '%.*s' needs a size, or an initializer to count",
                              name_length(node->name), node->name.text);
    }
    const bool is_constant = symbol->is_const && node->value != NULL && node->value->is_constant &&
                             symbol->storage != TB_SHADER_STORAGE_UNIFORM;
    if (is_constant)
    {
        symbol->has_value = true;
        symbol->value = node->value->constant;
    }
    symbol->type = type;
    node->checked = type;
    node->symbol = symbol;
    (void)tb_shader_check_declare(checker, symbol, node->location);
}

void tb_shader_check_variable(tb_shader_checker *checker, tb_shader_node *node)
{
    check_variable_in(checker, node, NULL);
}

static void check_buffer(tb_shader_checker *checker, tb_shader_node *node)
{
    for (tb_shader_node *member = node->children; member != NULL && !checker->stopped;
         member = member->next)
    {
        if (member->kind == TB_SHADER_NODE_VARIABLE)
        {
            check_variable_in(checker, member, node);
        }
        else
        {
            check_declaration(checker, member);
        }
    }
}

/* -------------------------------------------------------------------------
 * Declarations.
 * ------------------------------------------------------------------------- */

static void check_declaration(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_node_kind kind = node->kind;
    if (kind == TB_SHADER_NODE_NAMESPACE)
    {
        check_namespace(checker, node);
    }
    else if (kind == TB_SHADER_NODE_STRUCT || kind == TB_SHADER_NODE_INTERFACE)
    {
        check_struct(checker, node);
    }
    else if (kind == TB_SHADER_NODE_ENUM)
    {
        check_enum(checker, node);
    }
    else if (kind == TB_SHADER_NODE_TYPEDEF)
    {
        check_typedef(checker, node);
    }
    else if (kind == TB_SHADER_NODE_CBUFFER || kind == TB_SHADER_NODE_TBUFFER)
    {
        check_buffer(checker, node);
    }
    else if (kind == TB_SHADER_NODE_VARIABLE)
    {
        tb_shader_check_variable(checker, node);
    }
    else if (kind == TB_SHADER_NODE_FUNCTION)
    {
        check_function(checker, node);
    }
}

void tb_shader_check_declarations(tb_shader_checker *checker, tb_shader_node *first)
{
    for (tb_shader_node *node = first; node != NULL && !checker->stopped; node = node->next)
    {
        check_declaration(checker, node);
    }
}

void tb_shader_check_local_declaration(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node->kind == TB_SHADER_NODE_FUNCTION)
    {
        tb_shader_check_error(checker, node->location,
                              "a function cannot be declared in a function");
        return;
    }
    check_declaration(checker, node);
}
