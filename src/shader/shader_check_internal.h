/* The checker's state and the functions its files share. */
#ifndef TB_SHADER_CHECK_INTERNAL_H
#define TB_SHADER_CHECK_INTERNAL_H

#include "shader_check.h"

#include "badger/allocator.h"
#include "badger/array.h"
#include "badger/table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tb_shader_checker
{
    tb_arena *arena;
    tb_allocator allocator;
    tb_shader_types *types;
    const char *const *files;
    tb_array *diagnostics;
    size_t error_count;
    bool stopped;

    /* Names at namespace scope, by their qualified names. */
    tb_table *globals;
    /* Names in functions, innermost first, and what each hid. */
    tb_table *locals;
    tb_array *scope_changes;
    size_t scope_depth;
    /* The namespaces and structs being checked in: `N::S`; empty at the top. */
    tb_shader_name path;

    /* The function whose body is being checked, and its struct for a method. */
    tb_shader_symbol *function;
    const tb_shader_type *method_owner;
    size_t loop_depth;   /* Loops open: where `continue` may be. */
    size_t break_depth;  /* Loops and switches open: where `break` may be. */
    tb_shader_node *innermost_switch;
} tb_shader_checker;

/* Diagnostics. */
void tb_shader_check_error(tb_shader_checker *checker, tb_shader_location location,
                           const char *format, ...);
void tb_shader_check_warning(tb_shader_checker *checker, tb_shader_location location,
                             const char *format, ...);

/* Names. */
size_t tb_shader_check_push_scope(tb_shader_checker *checker);
void tb_shader_check_pop_scope(tb_shader_checker *checker, size_t mark);
/* Declare a symbol where the checker is: in the innermost scope of a
 * function, or at namespace scope by its qualified name, which it is given.
 * Reports a redefinition, and then returns false. */
bool tb_shader_check_declare(tb_shader_checker *checker, tb_shader_symbol *symbol,
                             tb_shader_location location);
/* What a name names where the checker is, or NULL. */
tb_shader_symbol *tb_shader_check_lookup(tb_shader_checker *checker, tb_shader_name name);
/* `left::right`, in the checker's arena. */
tb_shader_name tb_shader_check_join(tb_shader_checker *checker, tb_shader_name left,
                                    tb_shader_name right);
tb_shader_symbol *tb_shader_check_new_symbol(tb_shader_checker *checker, tb_shader_symbol_kind kind,
                                             tb_shader_name name, tb_shader_node *declaration);

/* Declarations (shader_check_declaration.c). */
void tb_shader_check_declarations(tb_shader_checker *checker, tb_shader_node *first);
/* The type a written type names; the error type after reporting why not. */
const tb_shader_type *tb_shader_check_type(tb_shader_checker *checker,
                                           const tb_shader_type_syntax *syntax);
/* A variable in a function, a struct, a buffer or at namespace scope. */
void tb_shader_check_variable(tb_shader_checker *checker, tb_shader_node *node);
/* A declaration in a statement: a variable, a struct, an enum or a typedef. */
void tb_shader_check_local_declaration(tb_shader_checker *checker, tb_shader_node *node);

/* Statements (shader_check_statement.c). */
void tb_shader_check_body(tb_shader_checker *checker, tb_shader_node *function);

/* Expressions (shader_check_expression.c). Each returns the node to keep in
 * the tree: the expression, or a CONVERT around it. */
tb_shader_node *tb_shader_check_expression(tb_shader_checker *checker, tb_shader_node *node);
/* Convert an expression to `type` implicitly, as an initializer, an argument
 * or a returned value is; reports what cannot be. */
tb_shader_node *tb_shader_check_convert(tb_shader_checker *checker, tb_shader_node *node,
                                        const tb_shader_type *type);
/* An initializer of a variable of `type`: an expression or a `{...}` list.
 * `length` receives the elements an unsized array gets from it. */
tb_shader_node *tb_shader_check_initializer(tb_shader_checker *checker, tb_shader_node *node,
                                            const tb_shader_type *type, uint32_t *length);
/* A condition of `if`, a loop or `?:`: a scalar, converted to bool. */
tb_shader_node *tb_shader_check_condition(tb_shader_checker *checker, tb_shader_node *node,
                                          const char *what);
/* An expression whose value must be a constant integer: an array's size, a
 * case's value, a template's argument. Returns false after reporting why not. */
bool tb_shader_check_constant_integer(tb_shader_checker *checker, tb_shader_node *node,
                                      int64_t *value);

/* Constants (shader_check_constant.c). */
/* Fold a checked expression whose operands are folded, when it is constant. */
void tb_shader_fold(tb_shader_checker *checker, tb_shader_node *node);
/* A constant converted to a scalar type. */
tb_shader_constant tb_shader_constant_convert(tb_shader_constant constant, tb_shader_scalar from,
                                              tb_shader_scalar to);

#endif
