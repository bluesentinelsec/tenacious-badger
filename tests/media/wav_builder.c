/* WAV files built byte by byte; see wav_builder.h. */
#include "wav_builder.h"

#include "badger/assert.h"

#include <string.h>

void wav_put(wav_builder *file, const void *bytes, size_t length)
{
    TB_ASSERT(length <= WAV_BUILDER_CAPACITY - file->length);
    if (length != 0)
    {
        memcpy(file->bytes + file->length, bytes, length);
    }
    file->length += length;
}

void wav_put_u16(wav_builder *file, uint32_t value)
{
    const unsigned char bytes[2] = {(unsigned char)(value & 0xFFu),
                                    (unsigned char)((value >> 8) & 0xFFu)};
    wav_put(file, bytes, sizeof(bytes));
}

void wav_put_u32(wav_builder *file, uint32_t value)
{
    wav_put_u16(file, value & 0xFFFFu);
    wav_put_u16(file, value >> 16);
}

void wav_put_u64(wav_builder *file, uint64_t value)
{
    wav_put_u32(file, (uint32_t)(value & 0xFFFFFFFFu));
    wav_put_u32(file, (uint32_t)(value >> 32));
}

void wav_patch_u32(wav_builder *file, size_t offset, uint32_t value)
{
    TB_ASSERT(offset + 4 <= file->length);
    file->bytes[offset] = (unsigned char)(value & 0xFFu);
    file->bytes[offset + 1] = (unsigned char)((value >> 8) & 0xFFu);
    file->bytes[offset + 2] = (unsigned char)((value >> 16) & 0xFFu);
    file->bytes[offset + 3] = (unsigned char)(value >> 24);
}

void wav_begin(wav_builder *file, const char *id)
{
    file->length = 0;
    wav_put(file, id, 4);
    wav_put_u32(file, 0);
    wav_put(file, "WAVE", 4);
}

size_t wav_open_chunk(wav_builder *file, const char *id)
{
    const size_t chunk = file->length;
    wav_put(file, id, 4);
    wav_put_u32(file, 0);
    return chunk;
}

void wav_close_chunk(wav_builder *file, size_t chunk)
{
    const size_t size = file->length - chunk - 8;
    wav_patch_u32(file, chunk + 4, (uint32_t)size);
    if (size % 2 != 0)
    {
        const unsigned char padding = 0;
        wav_put(file, &padding, 1);
    }
}

void wav_chunk(wav_builder *file, const char *id, const void *bytes, size_t length)
{
    const size_t chunk = wav_open_chunk(file, id);
    wav_put(file, bytes, length);
    wav_close_chunk(file, chunk);
}

void wav_fmt(wav_builder *file, uint32_t tag, uint32_t channels, uint32_t rate,
             uint32_t block_align, uint32_t bits)
{
    const size_t chunk = wav_open_chunk(file, "fmt ");
    wav_put_u16(file, tag);
    wav_put_u16(file, channels);
    wav_put_u32(file, rate);
    wav_put_u32(file, rate * block_align);
    wav_put_u16(file, block_align);
    wav_put_u16(file, bits);
    wav_close_chunk(file, chunk);
}

void wav_fmt_extensible(wav_builder *file, uint32_t sub_tag, uint32_t channels, uint32_t rate,
                        uint32_t block_align, uint32_t bits, uint32_t mask)
{
    static const unsigned char tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                           0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    const size_t chunk = wav_open_chunk(file, "fmt ");
    wav_put_u16(file, 0xFFFEu);
    wav_put_u16(file, channels);
    wav_put_u32(file, rate);
    wav_put_u32(file, rate * block_align);
    wav_put_u16(file, block_align);
    wav_put_u16(file, bits);
    wav_put_u16(file, 22);
    wav_put_u16(file, bits);
    wav_put_u32(file, mask);
    wav_put_u16(file, sub_tag);
    wav_put(file, tail, sizeof(tail));
    wav_close_chunk(file, chunk);
}

void wav_finish(wav_builder *file)
{
    wav_patch_u32(file, 4, (uint32_t)(file->length - 8));
}
