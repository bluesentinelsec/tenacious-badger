/* The engine's own shader files, which the engine library carries:
 * cmake/EmbedShaders.cmake writes every file under shaders/ into
 * tb_shader_engine_files, by its virtual path. */
#include "badger/arena.h"
#include "badger/shader.h"
#include "badger/str.h"

#include <stddef.h>
#include <string.h>

extern const tb_shader_file tb_shader_engine_files[];
extern const size_t tb_shader_engine_file_count;

static tb_shader_load_result load_engine_file(void *context, const char *path, size_t length,
                                              tb_arena *arena, const char **text,
                                              size_t *text_length)
{
    (void)context;
    (void)arena;
    for (size_t index = 0; index < tb_shader_engine_file_count; index++)
    {
        const tb_shader_file *const file = &tb_shader_engine_files[index];
        if (tb_str_length(file->path) == length && memcmp(file->path, path, length) == 0)
        {
            /* The library's own bytes outlive every compile. */
            *text = file->text;
            *text_length = file->length;
            return TB_SHADER_LOAD_OK;
        }
    }
    return TB_SHADER_LOAD_NOT_FOUND;
}

tb_shader_loader tb_shader_engine_loader(void)
{
    const tb_shader_loader loader = {load_engine_file, NULL};
    return loader;
}
