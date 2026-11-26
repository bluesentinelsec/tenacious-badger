/** @file wav_builder.h
 * @brief WAV files built byte by byte in tests, so that each case says
 * exactly which chunks and fields its file has, damaged ones included.
 *
 * @code
 * wav_builder file = {0};
 * wav_begin(&file, "RIFF");
 * wav_fmt(&file, 1, 2, 48000, 4, 16);
 * const size_t data = wav_open_chunk(&file, "data");
 * wav_put_u16(&file, 0x1234);
 * wav_close_chunk(&file, data);
 * wav_finish(&file);
 * tb_wav_read(file.bytes, file.length, NULL, &sound, &error);
 * @endcode
 *
 * @par Threading
 * Stage-owned: a builder belongs to the case that made it.
 */
#ifndef TB_WAV_BUILDER_H
#define TB_WAV_BUILDER_H

#include <stddef.h>
#include <stdint.h>

/** @brief Bytes a built file can have. */
#define WAV_BUILDER_CAPACITY 16384

/** @brief A file being built. Zero-initialize, then wav_begin(). */
typedef struct wav_builder
{
    unsigned char bytes[WAV_BUILDER_CAPACITY]; /**< The file. */
    size_t length;                             /**< Bytes of it so far. */
} wav_builder;

/** @brief Start a file: `id` ("RIFF", "RF64"), a size wav_finish() fills in, and "WAVE". */
void wav_begin(wav_builder *file, const char *id);

/** @brief Append bytes. */
void wav_put(wav_builder *file, const void *bytes, size_t length);
/** @brief Append a little-endian 16-bit value. */
void wav_put_u16(wav_builder *file, uint32_t value);
/** @brief Append a little-endian 32-bit value. */
void wav_put_u32(wav_builder *file, uint32_t value);
/** @brief Append a little-endian 64-bit value. */
void wav_put_u64(wav_builder *file, uint64_t value);

/** @brief Start a chunk; returns where its header is, for wav_close_chunk(). */
size_t wav_open_chunk(wav_builder *file, const char *id);
/** @brief Fill in a chunk's size from what was appended, and pad it to an even length. */
void wav_close_chunk(wav_builder *file, size_t chunk);
/** @brief A whole chunk of the bytes given, padded. */
void wav_chunk(wav_builder *file, const char *id, const void *bytes, size_t length);

/** @brief A `fmt ` chunk of 16 bytes. */
void wav_fmt(wav_builder *file, uint32_t tag, uint32_t channels, uint32_t rate,
             uint32_t block_align, uint32_t bits);

/** @brief A `fmt ` chunk of WAVE_FORMAT_EXTENSIBLE, of 40 bytes, with a sub-format tag. */
void wav_fmt_extensible(wav_builder *file, uint32_t sub_tag, uint32_t channels, uint32_t rate,
                        uint32_t block_align, uint32_t bits, uint32_t mask);

/** @brief Fill in the RIFF size: the bytes after the first 8. */
void wav_finish(wav_builder *file);

/** @brief Overwrite a little-endian 32-bit value at an offset. */
void wav_patch_u32(wav_builder *file, size_t offset, uint32_t value);

#endif
