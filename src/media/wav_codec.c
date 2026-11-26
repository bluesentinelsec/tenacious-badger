/* The WAV module's small parts: the compressed formats' decoders, G.711 and
 * IMA ADPCM, the names of results, and the error's message. */
#include "badger/wav.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/str.h"
#include "wav_internal.h"

#include <string.h>

const unsigned char tb_wav_subformat_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

/* -------------------------------------------------------------------------
 * G.711: each code is a sign, a segment of 3 bits, and 4 bits within it.
 * ------------------------------------------------------------------------- */

#define G711_SIGN 0x80u
#define G711_SEGMENT 0x70u
#define G711_SEGMENT_SHIFT 4u
#define G711_QUANT 0x0Fu
#define ALAW_TOGGLE 0x55u
#define MULAW_BIAS 0x84

int16_t tb_wav_alaw(unsigned char code)
{
    const unsigned toggled = code ^ ALAW_TOGGLE;
    const unsigned segment = (toggled & G711_SEGMENT) >> G711_SEGMENT_SHIFT;
    const int quant = (int)(toggled & G711_QUANT);
    int magnitude = 0;

    if (segment == 0)
    {
        magnitude = (quant * 2 + 1) << 3;
    }
    else
    {
        magnitude = (quant * 2 + 1 + 32) << (segment + 2);
    }
    return (int16_t)((toggled & G711_SIGN) != 0 ? magnitude : -magnitude);
}

int16_t tb_wav_mulaw(unsigned char code)
{
    const unsigned inverted = (unsigned)(~code) & 0xFFu;
    const unsigned segment = (inverted & G711_SEGMENT) >> G711_SEGMENT_SHIFT;
    const int biased = (((int)(inverted & G711_QUANT) << 3) + MULAW_BIAS) << segment;

    return (int16_t)((inverted & G711_SIGN) != 0 ? MULAW_BIAS - biased : biased - MULAW_BIAS);
}

/* -------------------------------------------------------------------------
 * IMA ADPCM.
 * ------------------------------------------------------------------------- */

#define IMA_STEPS 89
#define IMA_HEADER 4u
#define IMA_WORD 4u
#define IMA_SAMPLES_PER_WORD 8u

static const int16_t ima_steps[IMA_STEPS] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t ima_index_changes[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct ima_state
{
    int predictor;
    int index;
} ima_state;

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}

static int16_t ima_decode(ima_state *state, unsigned nibble)
{
    const int step = ima_steps[state->index];
    int difference = step >> 3;

    if ((nibble & 4u) != 0)
    {
        difference += step;
    }
    if ((nibble & 2u) != 0)
    {
        difference += step >> 1;
    }
    if ((nibble & 1u) != 0)
    {
        difference += step >> 2;
    }
    if ((nibble & 8u) != 0)
    {
        state->predictor -= difference;
    }
    else
    {
        state->predictor += difference;
    }
    state->predictor = clamp_int(state->predictor, INT16_MIN, INT16_MAX);
    state->index = clamp_int(state->index + ima_index_changes[nibble & 7u], 0, IMA_STEPS - 1);
    return (int16_t)state->predictor;
}

size_t tb_wav_decode_ima_block(const unsigned char *block, size_t length, uint32_t channels,
                               int16_t *frames)
{
    ima_state states[TB_SOUND_MAX_CHANNELS] = {0};
    const size_t header = (size_t)IMA_HEADER * channels;
    const size_t group = (size_t)IMA_WORD * channels;

    TB_ASSERT(channels >= 1 && channels <= TB_SOUND_MAX_CHANNELS);
    if (length < header)
    {
        return 0;
    }
    for (uint32_t channel = 0; channel < channels; channel++)
    {
        const unsigned char *const head = block + (size_t)IMA_HEADER * channel;
        states[channel].predictor = (int16_t)tb_wav_u16(head);
        /* An index past the table is damage a decoder survives by clamping. */
        states[channel].index = clamp_int(head[2], 0, IMA_STEPS - 1);
        frames[channel] = (int16_t)states[channel].predictor;
    }
    const size_t groups = (length - header) / group;
    for (size_t index = 0; index < groups; index++)
    {
        const unsigned char *const words = block + header + index * group;
        /* Frames 1 + 8 * index onwards. */
        int16_t *const base = frames + (1 + index * IMA_SAMPLES_PER_WORD) * channels;
        for (uint32_t channel = 0; channel < channels; channel++)
        {
            const unsigned char *const word = words + (size_t)IMA_WORD * channel;
            for (unsigned sample = 0; sample < IMA_SAMPLES_PER_WORD; sample++)
            {
                const unsigned byte = word[sample / 2];
                const unsigned nibble = (sample % 2 == 0) ? (byte & 0x0Fu) : (byte >> 4);
                base[(size_t)sample * channels + channel] = ima_decode(&states[channel], nibble);
            }
        }
    }
    return 1 + groups * IMA_SAMPLES_PER_WORD;
}

/* -------------------------------------------------------------------------
 * Results and errors.
 * ------------------------------------------------------------------------- */

const char *tb_wav_result_name(tb_wav_result result)
{
    switch (result)
    {
    case TB_WAV_OK:
        return "ok";
    case TB_WAV_NOT_FOUND:
        return "not found";
    case TB_WAV_INPUT_FAILED:
        return "input failed";
    case TB_WAV_MALFORMED:
        return "malformed";
    case TB_WAV_UNSUPPORTED:
        return "unsupported";
    case TB_WAV_TOO_LARGE:
        return "too large";
    case TB_WAV_NO_MEMORY:
        return "out of memory";
    case TB_WAV_OUTPUT_FAILED:
        return "output failed";
    case TB_WAV_UNSEEKABLE:
        return "cannot seek";
    }
    return "unknown result";
}

tb_wav_result tb_wav_fail(tb_wav_error *error, tb_wav_result result, const char *name,
                          const char *chunk, uint64_t offset, const char *reason)
{
    char where[64] = {0};
    char prefix[TB_WAV_ERROR_CAPACITY] = {0};

    if (error == NULL)
    {
        return result;
    }
    const size_t chunk_length = tb_str_length(chunk);
    *error = (tb_wav_error){
        .result = result, .offset = offset != TB_WAV_NOWHERE ? offset : 0, .reason = reason};
    memcpy(error->chunk, chunk, chunk_length < 4 ? chunk_length : 4);
    if (chunk[0] != '\0')
    {
        (void)tb_fmt_buffer(where, sizeof(where), "%s chunk at byte %llu: ", error->chunk,
                            (unsigned long long)offset);
    }
    else if (offset != TB_WAV_NOWHERE)
    {
        (void)tb_fmt_buffer(where, sizeof(where), "byte %llu: ", (unsigned long long)offset);
    }
    if (name != NULL)
    {
        (void)tb_fmt_buffer(prefix, sizeof(prefix), "%s: ", name);
    }
    (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s%s%s", prefix, where, reason);
    return result;
}
