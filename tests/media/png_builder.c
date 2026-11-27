#include "png_builder.h"

#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/memory.h"
#include "tb_test.h"

#include <string.h>

static void put_u32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static uint32_t get_u32(const uint8_t *at)
{
    return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

void png_build_begin(png_build *file)
{
    static const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};

    memset(file, 0, sizeof(*file));
    memcpy(file->bytes, signature, sizeof(signature));
    file->length = sizeof(signature);
}

void png_build_chunk(png_build *file, const char *name, const void *data, size_t length)
{
    TB_TEST_REQUIRE_UINT(file->length + 12u + length, <=, PNG_BUILD_CAPACITY);
    uint8_t *const at = file->bytes + file->length;
    put_u32(at, (uint32_t)length);
    memcpy(at + 4, name, 4);
    if (length > 0)
    {
        memcpy(at + 8, data, length);
    }
    put_u32(at + 8 + length, tb_crc32(at + 4, 4u + length));
    file->length += 12u + length;
}

void png_build_header(png_build *file, uint32_t width, uint32_t height, uint32_t depth,
                      uint32_t color, uint32_t interlace)
{
    uint8_t header[13] = {0};

    put_u32(header, width);
    put_u32(header + 4, height);
    header[8] = (uint8_t)depth;
    header[9] = (uint8_t)color;
    header[12] = (uint8_t)interlace;
    png_build_chunk(file, "IHDR", header, sizeof(header));
}

void png_build_data(png_build *file, const uint8_t *rows, size_t length, size_t split)
{
    const size_t capacity = tb_deflate_bound(length);
    uint8_t *const compressed = tb_malloc(capacity);
    size_t written = 0;

    TB_TEST_REQUIRE_UINT(
        tb_deflate(TB_DEFLATE_ZLIB, 9, rows, length, compressed, capacity, &written), ==,
        TB_DEFLATE_OK);
    if (split == 0)
    {
        split = written;
    }
    for (size_t at = 0; at < written; at += split)
    {
        const size_t piece = written - at < split ? written - at : split;
        png_build_chunk(file, "IDAT", compressed + at, piece);
    }
    tb_free(compressed);
}

void png_build_end(png_build *file)
{
    png_build_chunk(file, "IEND", NULL, 0);
}

void png_build_fix_crcs(png_build *file)
{
    size_t at = 8;

    while (file->length - at >= 12u)
    {
        const uint32_t length = get_u32(file->bytes + at);
        if (length > file->length - at - 12u)
        {
            return;
        }
        put_u32(file->bytes + at + 8u + length, tb_crc32(file->bytes + at + 4u, 4u + length));
        at += 12u + length;
    }
}

size_t png_build_find(const png_build *file, const char *name)
{
    size_t at = 8;

    while (file->length - at >= 12u)
    {
        const uint32_t length = get_u32(file->bytes + at);
        if (memcmp(file->bytes + at + 4u, name, 4) == 0)
        {
            return at;
        }
        if (length > file->length - at - 12u)
        {
            break;
        }
        at += 12u + length;
    }
    return SIZE_MAX;
}
