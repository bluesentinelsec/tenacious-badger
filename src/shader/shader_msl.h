/* Emit one extracted raster entry point as Metal Shading Language. Source
 * and diagnostics borrow the scratch arena until the compile copies them. */
#ifndef TB_SHADER_MSL_H
#define TB_SHADER_MSL_H
#include "badger/shader.h"
#include "shader_form.h"

bool tb_shader_write_msl(const tb_shader_form *form, const tb_shader_reflection *reflection,
                         tb_arena *arena, const char **source, size_t *size,
                         tb_shader_location *location, const char **reason);
#endif
