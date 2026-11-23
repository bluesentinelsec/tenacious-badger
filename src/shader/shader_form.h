/* The shader compiler's own form: what a checked shader is lowered into, and
 * what every backend's output is written from.
 *
 * A form is a module of types, global variables and functions. A function's
 * body is a tree of structured statements (blocks, `if`, `loop`, `switch`)
 * over an array of typed expressions, each computed once and used by its
 * index, as Naga and Tint do. Structured control flow is what HLSL, GLSL,
 * Metal and SPIR-V all need, and it tells the CPU where lanes that diverged
 * meet again. HLSL's conveniences (implicit conversions, compound
 * assignments, `out` arguments, short-circuit operators, swizzled stores) are
 * gone: each is written out in the form, so no backend handles them.
 *
 * Every part of a form is referred to by a 32-bit index, never a pointer, so
 * a form is cheap to copy, to serialize and to compare. Its matrices keep
 * HLSL's meaning: a `float3x4` has 3 rows of 4 columns, and indexing it gives
 * a row.
 *
 * Expressions are values, or pointers to a place: a local, a global, an
 * argument passed `out`, or an element or member of one. An expression is
 * evaluated where an EMIT statement names it, except those that only name
 * something (literals, arguments, locals and globals), which are evaluated
 * wherever they are used, and a call's result, which its CALL statement
 * gives. An expression may be used only after it is evaluated, in the block
 * that evaluated it or one inside that block.
 *
 * A form and everything in it live in the arena it was made in. */
#ifndef TB_SHADER_FORM_H
#define TB_SHADER_FORM_H

#include "shader_types.h"

#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/array.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An index that refers to nothing. */
#define TB_SHADER_FORM_NONE UINT32_MAX

/* -------------------------------------------------------------------------
 * Types.
 * ------------------------------------------------------------------------- */

typedef enum tb_shader_form_type_kind
{
    TB_SHADER_FORM_VOID,
    TB_SHADER_FORM_SCALAR,
    TB_SHADER_FORM_VECTOR,
    TB_SHADER_FORM_MATRIX,
    TB_SHADER_FORM_ARRAY,
    TB_SHADER_FORM_STRUCT,
    TB_SHADER_FORM_STRING /* Only a literal's, which only printf takes. */
} tb_shader_form_type_kind;

/* A type. Types are interned, so two are the same when their indices are;
 * each struct is a type of its own. */
typedef struct tb_shader_form_type
{
    tb_shader_form_type_kind kind;
    tb_shader_scalar scalar; /* SCALAR, VECTOR, MATRIX; never a literal. */
    uint32_t rows;           /* MATRIX */
    uint32_t columns;        /* VECTOR, MATRIX */
    uint32_t element;        /* ARRAY */
    uint32_t length;         /* ARRAY */
    uint32_t first_member;   /* STRUCT: into the form's members. */
    uint32_t member_count;   /* STRUCT */
    const char *name;        /* As HLSL writes it: `float3x4`, `Light`, `float[4]`. */
} tb_shader_form_type;

typedef struct tb_shader_form_member
{
    const char *name;
    uint32_t type;
} tb_shader_form_member;

/* -------------------------------------------------------------------------
 * Variables.
 * ------------------------------------------------------------------------- */

/* Where a pointer points; VALUE for an expression that is not a pointer. */
typedef enum tb_shader_form_space
{
    TB_SHADER_FORM_VALUE,
    TB_SHADER_FORM_FUNCTION,   /* A local, or an argument passed `out`. */
    TB_SHADER_FORM_PRIVATE,    /* A static global, one for each invocation. */
    TB_SHADER_FORM_GROUPSHARED /* Shared by a thread group. */
} tb_shader_form_space;

/* A global variable, which starts as zeros. */
typedef struct tb_shader_form_global
{
    const char *name;
    uint32_t type;
    tb_shader_form_space space; /* PRIVATE or GROUPSHARED */
} tb_shader_form_global;

/* A local variable, which starts undefined, as in HLSL. */
typedef struct tb_shader_form_local
{
    const char *name;
    uint32_t type;
} tb_shader_form_local;

typedef enum tb_shader_form_direction
{
    TB_SHADER_FORM_IN,
    TB_SHADER_FORM_OUT,  /* Its argument is a pointer to where the result goes. */
    TB_SHADER_FORM_INOUT /* As OUT, and what it points to holds the value passed. */
} tb_shader_form_direction;

typedef struct tb_shader_form_parameter
{
    const char *name;
    uint32_t type;
    tb_shader_form_direction direction;
} tb_shader_form_parameter;

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

typedef enum tb_shader_form_expression_kind
{
    TB_SHADER_FORM_LITERAL,       /* type, value: a scalar. */
    TB_SHADER_FORM_ARGUMENT,      /* index: the parameter; a pointer when it is OUT or INOUT. */
    TB_SHADER_FORM_LOCAL,         /* index: a pointer to the local. */
    TB_SHADER_FORM_GLOBAL,        /* index: a pointer to the global. */
    TB_SHADER_FORM_LOAD,          /* operands[0]: a pointer; the value it points to. */
    TB_SHADER_FORM_ACCESS,        /* operands[0]: an array, vector or matrix, or a pointer to
                                     one; operands[1]: an integer index. A pointer gives a pointer. */
    TB_SHADER_FORM_ACCESS_INDEX,  /* operands[0] as ACCESS; index: a constant, or a member. */
    TB_SHADER_FORM_SWIZZLE,       /* operands[0]: a vector value; swizzle: the components. */
    TB_SHADER_FORM_COMPOSE,       /* operand list: a vector's scalars and vectors, a matrix's
                                     rows, an array's elements, a struct's members. */
    TB_SHADER_FORM_SPLAT,         /* operands[0]: a scalar, made a vector or matrix. */
    TB_SHADER_FORM_UNARY,         /* op, operands[0] */
    TB_SHADER_FORM_BINARY,        /* op, operands[0], operands[1]: of one type. */
    TB_SHADER_FORM_SELECT,        /* operands: a bool scalar, the value if true, if false. */
    TB_SHADER_FORM_CONVERT,       /* operands[0]: the same shape, another component. */
    TB_SHADER_FORM_INTRINSIC,     /* intrinsic, operand list: a pure intrinsic's value. */
    TB_SHADER_FORM_CALL_RESULT,   /* What the CALL statement that gives it returned. */
    TB_SHADER_FORM_STRING_LITERAL /* index: into the form's strings. */
} tb_shader_form_expression_kind;

typedef enum tb_shader_form_op
{
    TB_SHADER_FORM_OP_NONE,
    /* Unary. */
    TB_SHADER_FORM_OP_NEGATE,
    TB_SHADER_FORM_OP_NOT, /* Of bools. */
    TB_SHADER_FORM_OP_BIT_NOT,
    /* Binary, component by component. */
    TB_SHADER_FORM_OP_ADD,
    TB_SHADER_FORM_OP_SUBTRACT,
    TB_SHADER_FORM_OP_MULTIPLY,
    TB_SHADER_FORM_OP_DIVIDE,
    TB_SHADER_FORM_OP_REMAINDER,
    TB_SHADER_FORM_OP_SHIFT_LEFT,
    TB_SHADER_FORM_OP_SHIFT_RIGHT,
    TB_SHADER_FORM_OP_BIT_AND,
    TB_SHADER_FORM_OP_BIT_OR,
    TB_SHADER_FORM_OP_BIT_XOR,
    TB_SHADER_FORM_OP_AND, /* Of bool scalars, both evaluated. */
    TB_SHADER_FORM_OP_OR,
    /* Comparisons: bools of the operands' shape. */
    TB_SHADER_FORM_OP_EQUAL,
    TB_SHADER_FORM_OP_NOT_EQUAL,
    TB_SHADER_FORM_OP_LESS,
    TB_SHADER_FORM_OP_LESS_EQUAL,
    TB_SHADER_FORM_OP_GREATER,
    TB_SHADER_FORM_OP_GREATER_EQUAL
} tb_shader_form_op;

/* The most components a swizzle selects. */
#define TB_SHADER_FORM_MAX_SWIZZLE 4

typedef struct tb_shader_form_expression
{
    tb_shader_form_expression_kind kind;
    uint32_t type; /* The value's, or what a pointer points to. */
    tb_shader_form_space space;
    tb_shader_form_op op;
    uint32_t operands[3];
    uint32_t first_operand; /* COMPOSE, INTRINSIC: into the function's operand list. */
    uint32_t operand_count;
    uint32_t index;     /* ARGUMENT, LOCAL, GLOBAL, ACCESS_INDEX */
    uint32_t intrinsic; /* INTRINSIC: a tb_shader_intrinsic. */
    uint8_t swizzle[TB_SHADER_FORM_MAX_SWIZZLE];
    uint8_t swizzle_count;
    tb_shader_constant value; /* LITERAL, of its type. */
} tb_shader_form_expression;

/* -------------------------------------------------------------------------
 * Statements.
 * ------------------------------------------------------------------------- */

typedef enum tb_shader_form_statement_kind
{
    TB_SHADER_FORM_BLOCK,  /* first: its first statement. */
    TB_SHADER_FORM_EMIT,   /* operands[0] to operands[1]: the expressions evaluated here. */
    TB_SHADER_FORM_IF,     /* operands[0]: a bool scalar; accept, reject: blocks, reject or NONE. */
    TB_SHADER_FORM_LOOP,   /* body, continuing: blocks; operands[0]: a bool scalar that the
                              continuing block evaluates, ending the loop when true, or NONE. */
    TB_SHADER_FORM_SWITCH, /* operands[0]: an integer scalar; first: its first CASE. */
    TB_SHADER_FORM_CASE,   /* value, or is_default; body: a block; falls_through. */
    TB_SHADER_FORM_BREAK,  /* Out of the innermost loop or switch. */
    TB_SHADER_FORM_CONTINUE, /* To the continuing block of the innermost loop. */
    TB_SHADER_FORM_RETURN,   /* operands[0]: the value, or NONE. */
    TB_SHADER_FORM_DISCARD,  /* The pixel is not written; execution goes on, as in DXIL. */
    TB_SHADER_FORM_STORE,    /* operands[0]: a pointer; operands[1]: the value to store. */
    TB_SHADER_FORM_CALL      /* function or intrinsic, operand list, result: a CALL_RESULT or
                                NONE. An OUT or INOUT argument is a pointer. */
} tb_shader_form_statement_kind;

typedef struct tb_shader_form_statement
{
    tb_shader_form_statement_kind kind;
    uint32_t next; /* The next statement of its block, or of its switch's cases. */
    uint32_t operands[2];
    uint32_t first;      /* BLOCK, SWITCH */
    uint32_t accept;     /* IF */
    uint32_t reject;     /* IF */
    uint32_t body;       /* LOOP, CASE */
    uint32_t continuing; /* LOOP */
    uint32_t function;   /* CALL of a function of the form, or NONE. */
    uint32_t intrinsic;  /* CALL of an intrinsic: a tb_shader_intrinsic, or 0. */
    uint32_t first_operand;
    uint32_t operand_count;
    uint32_t result; /* CALL */
    uint64_t value;  /* CASE, as the selector's bits. */
    bool is_default;
    bool falls_through;
} tb_shader_form_statement;

/* -------------------------------------------------------------------------
 * Functions and the module.
 * ------------------------------------------------------------------------- */

typedef struct tb_shader_form_function
{
    const char *name;
    uint32_t result;       /* A type; VOID for none. */
    tb_array *parameters;  /* tb_shader_form_parameter */
    tb_array *locals;      /* tb_shader_form_local */
    tb_array *expressions; /* tb_shader_form_expression */
    tb_array *operands;    /* uint32_t: the operand lists of expressions and calls. */
    tb_array *statements;  /* tb_shader_form_statement */
    uint32_t body;         /* A BLOCK. */
} tb_shader_form_function;

typedef struct tb_shader_form
{
    tb_arena *arena;
    tb_allocator allocator;
    tb_array *types;     /* tb_shader_form_type */
    tb_array *members;   /* tb_shader_form_member */
    tb_array *globals;   /* tb_shader_form_global */
    tb_array *functions; /* tb_shader_form_function */
    tb_array *strings;   /* const char *: each string literal's text, as written between quotes. */
    /* A function of no parameters that stores the first value of each
     * static variable that has one, in the order of the source; NONE when
     * there is none. Each entry point calls it before anything else. */
    uint32_t initializer;
} tb_shader_form;

/* An empty form in `arena`, which must outlive it. */
void tb_shader_form_init(tb_shader_form *form, tb_arena *arena);

/* Types, interned: the same arguments give the same index. */
uint32_t tb_shader_form_void(tb_shader_form *form);
uint32_t tb_shader_form_scalar(tb_shader_form *form, tb_shader_scalar scalar);
uint32_t tb_shader_form_vector(tb_shader_form *form, tb_shader_scalar scalar, uint32_t columns);
uint32_t tb_shader_form_matrix(tb_shader_form *form, tb_shader_scalar scalar, uint32_t rows,
                               uint32_t columns);
/* A scalar, a vector or a matrix of `scalar`, shaped as `shape` is. */
uint32_t tb_shader_form_shaped(tb_shader_form *form, tb_shader_scalar scalar, uint32_t shape);
uint32_t tb_shader_form_array(tb_shader_form *form, uint32_t element, uint32_t length);
uint32_t tb_shader_form_string(tb_shader_form *form);
/* A new struct of `count` members, copied. */
uint32_t tb_shader_form_struct(tb_shader_form *form, const char *name,
                               const tb_shader_form_member *members, uint32_t count);

/* The index of a scalar, vector or matrix already in the form, or NONE;
 * `rows` and `columns` are 1 for a scalar, and `rows` 1 for a vector. */
uint32_t tb_shader_form_find_numeric(const tb_shader_form *form, tb_shader_form_type_kind kind,
                                     tb_shader_scalar scalar, uint32_t rows, uint32_t columns);
/* `shape`'s kind and size with another component, if the form has it; NONE otherwise. */
uint32_t tb_shader_form_find_shaped(const tb_shader_form *form, tb_shader_scalar scalar,
                                    uint32_t shape);

const tb_shader_form_type *tb_shader_form_type_at(const tb_shader_form *form, uint32_t type);
const tb_shader_form_member *tb_shader_form_member_at(const tb_shader_form *form, uint32_t member);
/* Components of a scalar, vector or matrix; 0 for any other type. */
uint32_t tb_shader_form_component_count(const tb_shader_form *form, uint32_t type);
bool tb_shader_form_is_numeric(const tb_shader_form *form, uint32_t type);

uint32_t tb_shader_form_add_global(tb_shader_form *form, const tb_shader_form_global *global);
/* A string literal's text, copied; returns its index. */
uint32_t tb_shader_form_add_string(tb_shader_form *form, const char *text, size_t length);
const char *tb_shader_form_string_at(const tb_shader_form *form, uint32_t string);
/* A new function with no parameters and an empty body. */
uint32_t tb_shader_form_add_function(tb_shader_form *form, const char *name, uint32_t result);
tb_shader_form_function *tb_shader_form_function_at(const tb_shader_form *form, uint32_t function);
size_t tb_shader_form_function_count(const tb_shader_form *form);

uint32_t tb_shader_form_add_parameter(tb_shader_form_function *function,
                                      const tb_shader_form_parameter *parameter);
uint32_t tb_shader_form_add_local(tb_shader_form_function *function, const char *name,
                                  uint32_t type);
uint32_t tb_shader_form_add_expression(tb_shader_form_function *function,
                                       const tb_shader_form_expression *expression);
uint32_t tb_shader_form_add_statement(tb_shader_form_function *function,
                                      const tb_shader_form_statement *statement);
/* Copy `count` indices into the function's operand list; returns where they start. */
uint32_t tb_shader_form_add_operands(tb_shader_form_function *function, const uint32_t *operands,
                                     uint32_t count);

tb_shader_form_expression *tb_shader_form_expression_at(const tb_shader_form_function *function,
                                                        uint32_t expression);
tb_shader_form_statement *tb_shader_form_statement_at(const tb_shader_form_function *function,
                                                      uint32_t statement);
uint32_t tb_shader_form_operand_at(const tb_shader_form_function *function, uint32_t at);
size_t tb_shader_form_expression_count(const tb_shader_form_function *function);
size_t tb_shader_form_statement_count(const tb_shader_form_function *function);

/* Whether an expression is evaluated by an EMIT: false for those that only
 * name something, and for a call's result. */
bool tb_shader_form_is_emitted(tb_shader_form_expression_kind kind);

/* Check that a form keeps every rule of its kinds: indices in range, operand
 * types that agree, expressions evaluated before they are used, `break` and
 * `continue` where they belong. Returns false, and writes why into `why` (of
 * `size` bytes, NUL-terminated), at the first rule broken. For tests, and for
 * debug builds after each pass. */
bool tb_shader_form_verify(const tb_shader_form *form, char *why, size_t size);

/* A form as text, in `arena`, NUL-terminated, for tests and for
 * `badger-shaderc` to show. */
const char *tb_shader_form_print(const tb_shader_form *form, tb_arena *arena);

#endif
