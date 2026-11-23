/* Lowering: turning a checked syntax tree into the compiler's own form
 * (shader_form.h). Every conversion, short-circuit, compound assignment,
 * swizzled store and `out` argument of HLSL is written out, so what reads
 * the form next handles none of them. */
#ifndef TB_SHADER_LOWER_H
#define TB_SHADER_LOWER_H

#include "shader_check.h"
#include "shader_form.h"

/* What lowering reports: what the form cannot hold yet, as errors. */
typedef struct tb_shader_lowered
{
    tb_shader_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t error_count;
} tb_shader_lowered;

/* Lower a checked shader into `form`, all in `arena`, which must be the
 * arena the syntax lives in or one that outlives it. A shader that did not
 * check cleanly is not lowered. Each function with a body becomes one of the
 * form's, in the order of the source, and each static or groupshared global
 * one of its globals. Returns OK, or FAILED when the shader did not check or
 * uses what the form does not hold yet; the form then holds what was
 * lowered, and still keeps its rules. Running out of memory ends the
 * process. */
tb_shader_result tb_shader_lower(const tb_shader_syntax *syntax, const tb_shader_checked *checked,
                                 tb_arena *arena, tb_shader_form *form, tb_shader_lowered *lowered);

#endif
