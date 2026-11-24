/* Checking a shader: binding every name to what it names, deciding the type
 * of every expression, and reporting what HLSL does not allow, as DXC does.
 *
 * Checking annotates the syntax tree in place (the fields after "What
 * checking decides" in tb_shader_node) and inserts a CONVERT node wherever
 * a value is converted implicitly, so what reads the tree next sees every
 * conversion written out. */
#ifndef TB_SHADER_CHECK_H
#define TB_SHADER_CHECK_H

#include "shader_types.h"

/* A checked shader. */
typedef struct tb_shader_checked
{
    tb_shader_types *types;
    /* The syntax's diagnostics, then checking's, warnings among them. */
    tb_shader_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t error_count;
} tb_shader_checked;

/* Check a parsed shader, all in `arena`, which must be the arena the syntax
 * lives in or one that outlives it. `shader_model`, from TB_SHADER_MODEL(),
 * decides what is built in: 6.6 and later declare the descriptor heaps. A
 * syntax with errors is checked too, so one compile reports what it can, up
 * to TB_SHADER_MAX_ERRORS errors in all. Returns OK, or FAILED when the
 * syntax or checking has an error. Running out of memory ends the process. */
tb_shader_result tb_shader_check(tb_shader_syntax *syntax, unsigned shader_model, tb_arena *arena,
                                 tb_shader_checked *checked);

/* A tree as tb_shader_syntax_print() writes it, with each expression's type
 * after it: `(+ a:float 1:float):float`. For tests. */
const char *tb_shader_syntax_print_typed(const tb_shader_syntax *syntax, tb_arena *arena);

#endif
