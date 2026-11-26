/* What the shader module writes for D3D11, and the D3D11 backend reads: a
 * DXBC container, behind a table of how the backend binds what the shader
 * takes from the descriptor heaps.
 *
 * Shader Model 5 has no descriptor heaps. The compiler gives each resource
 * or sampler a shader takes from a heap a register of its own (`t`, `u` or
 * `s`), and says where the heap index that names it is: a 32-bit value at
 * an offset of one of the constant buffers the RHI binds (`b0`, the push
 * constants, `b1` and `b2`). Before each draw or dispatch, the backend reads
 * each index from the constants it was given, and binds what it names to
 * the register: bindless resources, emulated for indices that are the same
 * for the whole draw. An index the shader computes is refused when it
 * compiles.
 *
 * Every number is little-endian.
 *
 * | Offset | Bytes | Field |
 * | --- | --- | --- |
 * | 0 | 4 | `TB11` |
 * | 4 | 4 | The format's version: 1 |
 * | 8 | 4 | Slots of the table |
 * | 12 | 12 | Bytes the shader reads of `b0`, `b1` and `b2`, 4 each; 0 for one it does not read |
 * | 24 | 4 | Where the DXBC starts, from the start: after the table, at a multiple of 4 |
 * | 28 | 4 | Bytes of the DXBC |
 * | 32 | 12 each | The slots |
 *
 * A slot:
 *
 * | Offset | Bytes | Field |
 * | --- | --- | --- |
 * | 0 | 1 | The heap: 0 the resource heap, 1 the sampler heap |
 * | 1 | 1 | The register's class: `t`, `u` or `s` |
 * | 2 | 1 | What the shader declares there: a tb_shader_dxbc_view |
 * | 3 | 1 | The constant buffer the index is in: 0 for `b0` to 2 for `b2` |
 * | 4 | 4 | The index's offset in it, in bytes |
 * | 8 | 4 | The register |
 */
#ifndef TB_SHADER_DXBC_FORMAT_H
#define TB_SHADER_DXBC_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_SHADER_DXBC_MAGIC 0x31314254u /* "TB11" */
#define TB_SHADER_DXBC_VERSION 1u
#define TB_SHADER_DXBC_HEADER_BYTES 32u
#define TB_SHADER_DXBC_SLOT_BYTES 12u
/* The constant buffers the RHI binds: the push constants, then two more. */
#define TB_SHADER_DXBC_CONSTANT_BUFFERS 3u
/* Slots a shader has at most: the registers of D3D11's stages. */
#define TB_SHADER_DXBC_MAX_SLOTS 160u

/* The heaps of Shader Model 6.6 a slot stands for. */
#define TB_SHADER_DXBC_HEAP_RESOURCE 0u
#define TB_SHADER_DXBC_HEAP_SAMPLER 1u

/* What a shader declares at a slot's register, which the view the backend
 * binds there must be: D3D11 reads nothing through a view of another
 * dimension. */
typedef enum tb_shader_dxbc_view
{
    TB_SHADER_DXBC_VIEW_RAW_BUFFER,      /* ByteAddressBuffer, RWByteAddressBuffer */
    TB_SHADER_DXBC_VIEW_TEXTURE2D,       /* Texture2D, RWTexture2D */
    TB_SHADER_DXBC_VIEW_TEXTURE2D_ARRAY, /* Texture2DArray, RWTexture2DArray */
    TB_SHADER_DXBC_VIEW_TEXTURE_CUBE,    /* TextureCube */
    TB_SHADER_DXBC_VIEW_SAMPLER,         /* SamplerState, SamplerComparisonState */
    TB_SHADER_DXBC_VIEW_COUNT
} tb_shader_dxbc_view;

typedef struct tb_shader_dxbc_slot
{
    uint8_t heap;
    char register_class;
    uint8_t view;   /* tb_shader_dxbc_view */
    uint8_t source; /* 0 to 2: `b0` to `b2` */
    uint32_t offset;
    uint32_t register_index;
} tb_shader_dxbc_slot;

/* A shader for D3D11, read: its slots and its DXBC, which point into the
 * bytes it was read from. */
typedef struct tb_shader_dxbc_shader
{
    uint32_t constant_bytes[TB_SHADER_DXBC_CONSTANT_BUFFERS];
    tb_shader_dxbc_slot slots[TB_SHADER_DXBC_MAX_SLOTS];
    uint32_t slot_count;
    const void *code;
    size_t code_size;
} tb_shader_dxbc_shader;

/* Read a shader for D3D11 from `bytes`: false when they are not one, of
 * this version, whole and consistent. */
bool tb_shader_dxbc_read(const void *bytes, size_t size, tb_shader_dxbc_shader *shader);

#endif
