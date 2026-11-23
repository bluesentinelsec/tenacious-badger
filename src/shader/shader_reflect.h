/* Reflection: what a stage takes, gives and binds, read from the form of
 * one entry point (tb_shader_form_extract()). */
#ifndef TB_SHADER_REFLECT_H
#define TB_SHADER_REFLECT_H

#include "shader_form.h"

#include "badger/arena.h"
#include "badger/shader.h"

/* Describe the one entry point of `form`, all in `arena`. Returns NULL, or
 * a message, in `arena`, saying why the entry point cannot run: a value it
 * passes without a semantic, an array of resources of no size without a
 * register. */
const char *tb_shader_reflect(const tb_shader_form *form, tb_arena *arena,
                              tb_shader_reflection *reflection);

#endif
