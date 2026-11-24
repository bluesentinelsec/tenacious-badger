/* The numbers of the shader cache's entries and of shader libraries, which
 * are little-endian on every system, whatever its own order. */
#ifndef TB_SHADER_BYTES_H
#define TB_SHADER_BYTES_H

#include <stdint.h>

static inline void tb_shader_store_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++)
    {
        bytes[index] = (uint8_t)(value >> (index * 8u));
    }
}

static inline void tb_shader_store_u64(uint8_t *bytes, uint64_t value)
{
    for (unsigned index = 0; index < 8; index++)
    {
        bytes[index] = (uint8_t)(value >> (index * 8u));
    }
}

static inline uint32_t tb_shader_load_u32(const uint8_t *bytes)
{
    uint32_t value = 0;
    for (unsigned index = 0; index < 4; index++)
    {
        value |= (uint32_t)bytes[index] << (index * 8u);
    }
    return value;
}

static inline uint64_t tb_shader_load_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; index++)
    {
        value |= (uint64_t)bytes[index] << (index * 8u);
    }
    return value;
}

#endif
