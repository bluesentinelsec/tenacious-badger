/* The shader compiler's syntax tree: what the parser makes of the
 * preprocessed tokens of a shader, and what type checking reads.
 *
 * The tree records what the source says, not what it means: a name is not
 * yet bound to what it names, and a type is a name, not a type. Every node
 * and every string of a tree lives in the arena it was parsed into. */
#ifndef TB_SHADER_SYNTAX_H
#define TB_SHADER_SYNTAX_H

#include "shader_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A name as the source spells it, qualified ones included (`N::Light`), and
 * operator names (`operator+`). Not NUL-terminated. */
typedef struct tb_shader_name
{
    const char *text;
    size_t length;
} tb_shader_name;

typedef struct tb_shader_node tb_shader_node;
typedef struct tb_shader_type_syntax tb_shader_type_syntax;

/* Modifiers of a declaration, as a bit set. The contextual ones (`point`,
 * `sample`, `indices`, ...) are modifiers only where one can be: before the
 * type of a declaration. */
#define TB_SHADER_MODIFIER_STATIC (UINT64_C(1) << 0)
#define TB_SHADER_MODIFIER_CONST (UINT64_C(1) << 1)
#define TB_SHADER_MODIFIER_UNIFORM (UINT64_C(1) << 2)
#define TB_SHADER_MODIFIER_EXTERN (UINT64_C(1) << 3)
#define TB_SHADER_MODIFIER_VOLATILE (UINT64_C(1) << 4)
#define TB_SHADER_MODIFIER_GROUPSHARED (UINT64_C(1) << 5)
#define TB_SHADER_MODIFIER_PRECISE (UINT64_C(1) << 6)
#define TB_SHADER_MODIFIER_SHARED (UINT64_C(1) << 7)
#define TB_SHADER_MODIFIER_INLINE (UINT64_C(1) << 8)
#define TB_SHADER_MODIFIER_EXPORT (UINT64_C(1) << 9)
#define TB_SHADER_MODIFIER_ROW_MAJOR (UINT64_C(1) << 10)
#define TB_SHADER_MODIFIER_COLUMN_MAJOR (UINT64_C(1) << 11)
#define TB_SHADER_MODIFIER_SNORM (UINT64_C(1) << 12)
#define TB_SHADER_MODIFIER_UNORM (UINT64_C(1) << 13)
#define TB_SHADER_MODIFIER_IN (UINT64_C(1) << 14)
#define TB_SHADER_MODIFIER_OUT (UINT64_C(1) << 15)
#define TB_SHADER_MODIFIER_INOUT (UINT64_C(1) << 16)
#define TB_SHADER_MODIFIER_NOINTERPOLATION (UINT64_C(1) << 17)
#define TB_SHADER_MODIFIER_LINEAR (UINT64_C(1) << 18)
#define TB_SHADER_MODIFIER_CENTROID (UINT64_C(1) << 19)
#define TB_SHADER_MODIFIER_NOPERSPECTIVE (UINT64_C(1) << 20)
#define TB_SHADER_MODIFIER_SAMPLE (UINT64_C(1) << 21)
#define TB_SHADER_MODIFIER_GLOBALLYCOHERENT (UINT64_C(1) << 22)
#define TB_SHADER_MODIFIER_POINT (UINT64_C(1) << 23)
#define TB_SHADER_MODIFIER_LINE (UINT64_C(1) << 24)
#define TB_SHADER_MODIFIER_TRIANGLE (UINT64_C(1) << 25)
#define TB_SHADER_MODIFIER_LINEADJ (UINT64_C(1) << 26)
#define TB_SHADER_MODIFIER_TRIANGLEADJ (UINT64_C(1) << 27)
#define TB_SHADER_MODIFIER_INDICES (UINT64_C(1) << 28)
#define TB_SHADER_MODIFIER_VERTICES (UINT64_C(1) << 29)
#define TB_SHADER_MODIFIER_PRIMITIVES (UINT64_C(1) << 30)
#define TB_SHADER_MODIFIER_PAYLOAD (UINT64_C(1) << 31)

/* Its spelling, for the printer and for diagnostics: "row_major", or NULL
 * when `modifier` is not one bit of the set. */
const char *tb_shader_modifier_name(uint64_t modifier);

/* `[name(arguments)]` before a declaration or a statement, and
 * `[[ns::name(arguments)]]`, whose name keeps its namespace. */
typedef struct tb_shader_attribute
{
    tb_shader_location location;
    tb_shader_name name;
    tb_shader_node *arguments; /* Expressions, linked by `next`. */
    size_t argument_count;
    struct tb_shader_attribute *next;
} tb_shader_attribute;

/* What follows a `:` in a declaration: a semantic (`SV_Position`), or a
 * clause with arguments: `register(t0, space1)`, `packoffset(c1.y)`, and a
 * ray payload's `read(caller)` and `write(closesthit)`. */
typedef struct tb_shader_annotation
{
    tb_shader_location location;
    tb_shader_name name;
    bool has_arguments;        /* Written with parentheses, even empty ones. */
    tb_shader_node *arguments; /* Expressions, linked by `next`. */
    size_t argument_count;
    struct tb_shader_annotation *next;
} tb_shader_annotation;

/* One argument of a template: `float4` of `Texture2D<float4>`, `4` of
 * `vector<float, 4>`. Exactly one of the two is set. */
typedef struct tb_shader_template_argument
{
    tb_shader_type_syntax *type;
    tb_shader_node *value;
    struct tb_shader_template_argument *next;
} tb_shader_template_argument;

typedef enum tb_shader_type_syntax_kind
{
    TB_SHADER_TYPE_SYNTAX_NAMED, /* A type by its name, with template arguments if it has any. */
    TB_SHADER_TYPE_SYNTAX_ARRAY  /* An array of `element`, of `size` elements or unsized. */
} tb_shader_type_syntax_kind;

/* A type as the source writes it. */
struct tb_shader_type_syntax
{
    tb_shader_type_syntax_kind kind;
    tb_shader_location location;
    /* NAMED: the name; empty for a struct or enum defined in place without one. */
    tb_shader_name name;
    tb_shader_template_argument *arguments; /* NAMED: linked by `next`. */
    size_t argument_count;
    bool has_arguments; /* NAMED: written with `<...>`. */
    /* NAMED: snorm, unorm, row_major and column_major written inside template
     * arguments (`RWTexture2D<unorm float4>`). A declaration's own modifiers
     * are on the declaration. */
    uint64_t modifiers;
    /* NAMED: the struct, interface or enum this type defines in place, which
     * is also a declaration of its own, before the one that names it. */
    tb_shader_node *definition;
    tb_shader_type_syntax *element; /* ARRAY */
    tb_shader_node *size;           /* ARRAY: the expression, or NULL for `[]`. */
};

typedef enum tb_shader_node_kind
{
    /* Declarations. */
    TB_SHADER_NODE_STRUCT,     /* struct or class: name, type (its base), attributes, children. */
    TB_SHADER_NODE_INTERFACE,  /* name, children: its methods. */
    TB_SHADER_NODE_FUNCTION,   /* name, type (returned), parameters, annotations, body (NULL for a
                                  prototype). */
    TB_SHADER_NODE_VARIABLE,   /* name, type, annotations, value (initializer or bit width). */
    TB_SHADER_NODE_PARAMETER,  /* name (may be empty), type, annotations, value (default). */
    TB_SHADER_NODE_CBUFFER,    /* name, annotations, children: its variables and types. */
    TB_SHADER_NODE_TBUFFER,    /* As CBUFFER. */
    TB_SHADER_NODE_TYPEDEF,    /* name, type. */
    TB_SHADER_NODE_NAMESPACE,  /* name, children. */
    TB_SHADER_NODE_ENUM,       /* name, type (underlying, or NULL), children: enumerators. */
    TB_SHADER_NODE_ENUMERATOR, /* name, value (or NULL). */
    TB_SHADER_NODE_TEMPLATE_PARAMETER, /* name: `typename T` or `class T`. */
    TB_SHADER_NODE_PRAGMA,             /* name: what follows `#pragma`. */

    /* Statements. */
    TB_SHADER_NODE_BLOCK,                /* children */
    TB_SHADER_NODE_DECLARATION,          /* children: the variables, types and typedefs declared. */
    TB_SHADER_NODE_EXPRESSION_STATEMENT, /* value */
    TB_SHADER_NODE_IF,                   /* condition, body, else_body (or NULL). */
    TB_SHADER_NODE_FOR,   /* init (a statement, or NULL), condition, step, body; any may be NULL but
                             body. */
    TB_SHADER_NODE_WHILE, /* condition, body */
    TB_SHADER_NODE_DO,    /* body, condition */
    TB_SHADER_NODE_SWITCH, /* condition, body */
    TB_SHADER_NODE_CASE,   /* value; the statements after it, up to the next label, are its own. */
    TB_SHADER_NODE_DEFAULT,
    TB_SHADER_NODE_BREAK,
    TB_SHADER_NODE_CONTINUE,
    TB_SHADER_NODE_RETURN, /* value (or NULL) */
    TB_SHADER_NODE_DISCARD,
    TB_SHADER_NODE_EMPTY, /* `;` */

    /* Expressions. */
    TB_SHADER_NODE_INTEGER,     /* name: the spelling; integer: the value; suffix */
    TB_SHADER_NODE_FLOAT,       /* name: the spelling; suffix */
    TB_SHADER_NODE_BOOL,        /* integer: 1 for true */
    TB_SHADER_NODE_STRING,      /* name: the spelling, quotes included */
    TB_SHADER_NODE_NAME,        /* name, template_arguments (`f<float>`) */
    TB_SHADER_NODE_UNARY,       /* op, left */
    TB_SHADER_NODE_BINARY,      /* op, left, right; the comma operator included */
    TB_SHADER_NODE_ASSIGN,      /* op, left, right */
    TB_SHADER_NODE_CONDITIONAL, /* condition, left (if true), right (if false) */
    TB_SHADER_NODE_CALL,        /* left (what is called), children: arguments */
    TB_SHADER_NODE_CONSTRUCT,   /* type, children: arguments, as `float4(a, 1)` */
    TB_SHADER_NODE_CAST,        /* type, left */
    TB_SHADER_NODE_INDEX,       /* left, right */
    TB_SHADER_NODE_MEMBER, /* left, name (a member, a method or a swizzle), template_arguments */
    TB_SHADER_NODE_INITIALIZER_LIST, /* children */
    TB_SHADER_NODE_SIZEOF,           /* type, or left */
    /* Made by checking, not by the parser: left converted to `checked`. */
    TB_SHADER_NODE_CONVERT
} tb_shader_node_kind;

typedef enum tb_shader_operator
{
    TB_SHADER_OP_NONE,
    /* Unary. */
    TB_SHADER_OP_PLUS,
    TB_SHADER_OP_NEGATE,
    TB_SHADER_OP_NOT,
    TB_SHADER_OP_BIT_NOT,
    TB_SHADER_OP_PRE_INCREMENT,
    TB_SHADER_OP_PRE_DECREMENT,
    TB_SHADER_OP_POST_INCREMENT,
    TB_SHADER_OP_POST_DECREMENT,
    /* Binary. */
    TB_SHADER_OP_MULTIPLY,
    TB_SHADER_OP_DIVIDE,
    TB_SHADER_OP_REMAINDER,
    TB_SHADER_OP_ADD,
    TB_SHADER_OP_SUBTRACT,
    TB_SHADER_OP_SHIFT_LEFT,
    TB_SHADER_OP_SHIFT_RIGHT,
    TB_SHADER_OP_LESS,
    TB_SHADER_OP_GREATER,
    TB_SHADER_OP_LESS_EQUAL,
    TB_SHADER_OP_GREATER_EQUAL,
    TB_SHADER_OP_EQUAL,
    TB_SHADER_OP_NOT_EQUAL,
    TB_SHADER_OP_BIT_AND,
    TB_SHADER_OP_BIT_XOR,
    TB_SHADER_OP_BIT_OR,
    TB_SHADER_OP_AND,
    TB_SHADER_OP_OR,
    TB_SHADER_OP_COMMA,
    /* Assignment. */
    TB_SHADER_OP_ASSIGN,
    TB_SHADER_OP_MULTIPLY_ASSIGN,
    TB_SHADER_OP_DIVIDE_ASSIGN,
    TB_SHADER_OP_REMAINDER_ASSIGN,
    TB_SHADER_OP_ADD_ASSIGN,
    TB_SHADER_OP_SUBTRACT_ASSIGN,
    TB_SHADER_OP_SHIFT_LEFT_ASSIGN,
    TB_SHADER_OP_SHIFT_RIGHT_ASSIGN,
    TB_SHADER_OP_BIT_AND_ASSIGN,
    TB_SHADER_OP_BIT_XOR_ASSIGN,
    TB_SHADER_OP_BIT_OR_ASSIGN
} tb_shader_operator;

/* Its spelling: "+=", "!", and "++" for both increments. */
const char *tb_shader_operator_spelling(tb_shader_operator op);

struct tb_shader_type;
struct tb_shader_symbol;

/* A constant value: of an integer, a bool or a floating-point type. */
typedef struct tb_shader_constant
{
    bool is_float;
    uint64_t bits; /* Integers and bools, as two's complement in 64 bits. */
    double value;  /* Floating point, rounded to its type. */
} tb_shader_constant;

/* One node of the tree. Which fields a kind uses is listed with the kind;
 * the others are zero. */
struct tb_shader_node
{
    tb_shader_node_kind kind;
    tb_shader_location location;
    tb_shader_node *next; /* The next node of the list this one is in. */

    tb_shader_name name;
    tb_shader_type_syntax *type;
    uint64_t modifiers; /* TB_SHADER_MODIFIER_* */
    tb_shader_attribute *attributes;
    tb_shader_annotation *annotations;
    /* STRUCT and FUNCTION declared with `template<...>`: TEMPLATE_PARAMETER
     * nodes, and PARAMETER nodes for values. */
    tb_shader_node *template_parameters;
    tb_shader_template_argument *template_arguments;
    bool is_template;  /* Declared with `template<...>`, even an empty list. */
    bool is_scoped;    /* ENUM: `enum class`. */
    bool is_bit_field; /* VARIABLE: value is a bit width, not an initializer. */

    tb_shader_node *children;
    size_t child_count;
    tb_shader_node *parameters; /* FUNCTION */
    size_t parameter_count;

    tb_shader_operator op;
    tb_shader_node *left;
    tb_shader_node *right;
    tb_shader_node *condition;
    tb_shader_node *init;
    tb_shader_node *step;
    tb_shader_node *body;
    tb_shader_node *else_body;
    tb_shader_node *value;

    /* Expressions: how many nodes the longest path down from this one
     * passes, this one included; at most TB_SHADER_MAX_EXPRESSION_HEIGHT. */
    uint32_t height;

    uint64_t integer;      /* INTEGER, BOOL */
    tb_shader_name suffix; /* INTEGER, FLOAT: `u`, `ul`, `f`, `h`, `l`; may be empty. */

    /* What checking decides (shader_check.h); zero until then. */
    /* Expressions: the type of the value. Declarations: the type declared,
     * and a function's: the type it returns. */
    const struct tb_shader_type *checked;
    /* NAME: what it names. MEMBER: the field or method. CALL: the function
     * called. Declarations: the symbol declared. */
    struct tb_shader_symbol *symbol;
    uint32_t intrinsic; /* CALL of an intrinsic: its tb_shader_intrinsic; 0 otherwise. */
    uint32_t method; /* CALL of a method of a built-in object: its tb_shader_method; 0 otherwise. */
    /* CALL: the type of each parameter the arguments were passed to, one for
     * each argument but a variadic one's, and which arguments are outputs
     * (`out` or `inout`), bit i for argument i. */
    const struct tb_shader_type *const *parameter_types;
    uint64_t outputs;
    bool is_lvalue;     /* Expressions: names a place, which may still be const. */
    bool is_assignable; /* Expressions: a place that may be written. */
    bool is_constant;   /* Expressions: its value is known: `constant`. */
    tb_shader_constant constant;
    /* MEMBER that is a swizzle: the components it selects; a matrix's as
     * row * 4 + column. */
    uint8_t swizzle[4];
    uint8_t swizzle_count;
};

/* A parsed shader. */
typedef struct tb_shader_syntax
{
    tb_shader_node *declarations; /* Linked by `next`, in the order of the source. */
    size_t declaration_count;
    const char *const *files; /* As in the unit parsed. */
    size_t file_count;
    /* The unit's diagnostics, then the parser's. */
    tb_shader_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t error_count;
} tb_shader_syntax;

/* Parse the tokens of a preprocessed unit into a tree, all in `arena`, which
 * must outlive the tree. The parser recovers from an error at the next
 * statement or declaration, so one compile reports what it can, up to
 * TB_SHADER_MAX_ERRORS errors counted with the unit's. Nesting deeper than
 * TB_SHADER_MAX_SYNTAX_NESTING is an error that ends the parse, so no source
 * can exhaust the stack. Returns OK, or FAILED when the unit or the parse has
 * an error; the tree holds what was read in both cases. Running out of
 * memory ends the process. */
tb_shader_result tb_shader_parse(const tb_shader_unit *unit, tb_arena *arena,
                                 tb_shader_syntax *syntax);

/* How deep statements, expressions and types may nest in one another. */
#define TB_SHADER_MAX_SYNTAX_NESTING ((size_t)256)

/* How tall an expression's tree may be. Operators chained without
 * parentheses (`a + b + c`) nest in the tree but not in the parser, so this
 * bounds them too, for whatever walks the tree recursively. */
#define TB_SHADER_MAX_EXPRESSION_HEIGHT ((uint32_t)1024)

/* A tree as text, in `arena`, NUL-terminated: one line for each declaration,
 * as S-expressions, for tests and for `badger-shaderc` to show. */
const char *tb_shader_syntax_print(const tb_shader_syntax *syntax, tb_arena *arena);

#endif
