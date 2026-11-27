/* Buffer slots shared by MSL entry points and the Metal command encoders.
 * The first three hold b0, b1 and b2. Each argument buffer is an indexed
 * table of GPU addresses (raw buffers) or Metal resource IDs. */
#ifndef TB_SHADER_MSL_BINDINGS_H
#define TB_SHADER_MSL_BINDINGS_H

typedef enum tb_msl_heap
{
    TB_MSL_BUFFERS,
    TB_MSL_TEXTURES,
    TB_MSL_ARRAY_TEXTURES,
    TB_MSL_CUBES,
    TB_MSL_STORAGE_TEXTURES,
    TB_MSL_STORAGE_ARRAYS,
    TB_MSL_SAMPLERS,
    TB_MSL_HEAP_COUNT
} tb_msl_heap;

#define TB_MSL_FIRST_HEAP_SLOT 3u
#define TB_MSL_FIRST_VERTEX_SLOT 16u
#define TB_MSL_RESOURCE_COUNT 65536u
#define TB_MSL_SAMPLER_COUNT 2048u
#endif
