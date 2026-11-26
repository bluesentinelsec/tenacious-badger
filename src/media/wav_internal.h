/* What the WAV reader and writer share: the format's numbers, little-endian
 * fields, the decoders of the compressed formats, and the error's message. */
#ifndef TB_WAV_INTERNAL_H
#define TB_WAV_INTERNAL_H

#include "badger/wav.h"

#include <stddef.h>
#include <stdint.h>

#define TB_WAV_FORMAT_PCM 0x0001u
#define TB_WAV_FORMAT_IEEE_FLOAT 0x0003u
#define TB_WAV_FORMAT_ALAW 0x0006u
#define TB_WAV_FORMAT_MULAW 0x0007u
#define TB_WAV_FORMAT_IMA_ADPCM 0x0011u
#define TB_WAV_FORMAT_EXTENSIBLE 0xFFFEu

/* Bytes of a chunk's header, and of the fields of each chunk this module reads or writes. */
#define TB_WAV_CHUNK_HEADER 8u
#define TB_WAV_FMT_SIZE 16u
#define TB_WAV_FMT_FLOAT_SIZE 18u
#define TB_WAV_FMT_EXTENSIBLE_SIZE 40u
#define TB_WAV_EXTENSIBLE_EXTRA 22u
#define TB_WAV_SMPL_HEADER 36u
#define TB_WAV_SMPL_LOOP 24u

/* The bytes of KSDATAFORMAT_SUBTYPE_* after the format tag, the same for every tag. */
extern const unsigned char tb_wav_subformat_tail[14];

static inline uint16_t tb_wav_u16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static inline uint32_t tb_wav_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static inline uint64_t tb_wav_u64(const unsigned char *bytes)
{
    return (uint64_t)tb_wav_u32(bytes) | ((uint64_t)tb_wav_u32(bytes + 4) << 32);
}

static inline void tb_wav_put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)(value >> 8);
}

static inline void tb_wav_put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFu);
    bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
    bytes[2] = (unsigned char)((value >> 16) & 0xFFu);
    bytes[3] = (unsigned char)(value >> 24);
}

/* A sample of G.711 A-law or mu-law, expanded to 16 bits. */
int16_t tb_wav_alaw(unsigned char code);
int16_t tb_wav_mulaw(unsigned char code);

/*
 * Decode one block of IMA ADPCM of `channels` channels, of `length` bytes,
 * into interleaved 16-bit frames. A block is, for each channel, a header of
 * a 16-bit predictor and a step index, then words of 4 bytes, each 8
 * samples of one channel, the channels in turn. Returns the frames decoded:
 * 1 for the headers and 8 for each whole group of words; 0 when `length`
 * is shorter than the headers. `frames` holds as many as the block can.
 */
size_t tb_wav_decode_ima_block(const unsigned char *block, size_t length, uint32_t channels,
                               int16_t *frames);

/* The offset of a failure that is at no place in the file, such as one to open it. */
#define TB_WAV_NOWHERE UINT64_MAX

/* Fill an error, with `name` before the rest of its message when there is one. */
tb_wav_result tb_wav_fail(tb_wav_error *error, tb_wav_result result, const char *name,
                          const char *chunk, uint64_t offset, const char *reason);

#endif
