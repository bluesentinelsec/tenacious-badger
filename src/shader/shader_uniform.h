/* Locate draw-uniform heap indices for backends with per-stage resource slots. */
#ifndef TB_SHADER_UNIFORM_H
#define TB_SHADER_UNIFORM_H
#include "shader_hlsl.h"
const tb_shader_binding *tb_shader_binding_named(const tb_shader_reflection *reflection,
                                                 const char *name);
bool tb_shader_uniform_index(const tb_shader_hlsl_request *request,
                             const tb_shader_form_function *function, uint32_t expression,
                             uint32_t *buffer, uint32_t *offset);
#endif
