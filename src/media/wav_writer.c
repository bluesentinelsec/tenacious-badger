/* Writing WAV files: a sound's samples in its own format, inside
 * WAVE_FORMAT_EXTENSIBLE where the format's documentation requires it, with
 * a `fact` chunk for float samples and a `smpl` chunk for loops. Every size
 * is known before the first byte, so the stream need not seek. */
#include "badger/wav.h"

#include "badger/assert.h"
#include "sound_internal.h"
#include "wav_internal.h"

#include <stdbool.h>
#include <string.h>

/* Bytes of samples converted to the file's byte order at a time. */
#define SCRATCH_SIZE 4096u
/* The sample period of `smpl`, in nanoseconds, is a second over the rate. */
#define NANOSECONDS_PER_SECOND 1000000000u
/* The MIDI note `smpl` says the samples are recorded at: middle C. */
#define MIDI_MIDDLE_C 60u

/* How the `fmt ` chunk describes the sound. */
typedef struct wav_layout
{
    uint16_t tag; /* The format tag: PCM or IEEE float. */
    bool extensible;
    uint32_t fmt_size;
    bool has_fact;
    uint32_t smpl_size; /* 0 for none. */
    uint64_t data_size;
} wav_layout;

static wav_layout plan(const tb_sound *sound)
{
    wav_layout layout = {0};
    const tb_sound_spec *const spec = &sound->spec;
    const bool is_float = spec->format == TB_SOUND_FORMAT_F32;
    const bool default_mask = spec->channel_mask == 0 ||
                              spec->channel_mask == tb_sound_default_channel_mask(spec->channels);

    layout.tag = is_float ? TB_WAV_FORMAT_IEEE_FLOAT : TB_WAV_FORMAT_PCM;
    layout.extensible = spec->channels > 2 || !default_mask ||
                        (!is_float && tb_sound_format_size(spec->format) > 2);
    if (layout.extensible)
    {
        layout.fmt_size = TB_WAV_FMT_EXTENSIBLE_SIZE;
    }
    else if (is_float)
    {
        layout.fmt_size = TB_WAV_FMT_FLOAT_SIZE;
    }
    else
    {
        layout.fmt_size = TB_WAV_FMT_SIZE;
    }
    layout.has_fact = is_float;
    if (sound->loop_count != 0)
    {
        layout.smpl_size = TB_WAV_SMPL_HEADER + TB_WAV_SMPL_LOOP * (uint32_t)sound->loop_count;
    }
    layout.data_size = (uint64_t)sound->frame_count * sound->frame_size;
    return layout;
}

/* The RIFF size: everything after the first 8 bytes. */
static uint64_t riff_size(const wav_layout *layout)
{
    uint64_t size = 4 + TB_WAV_CHUNK_HEADER + layout->fmt_size;
    if (layout->has_fact)
    {
        size += TB_WAV_CHUNK_HEADER + 4;
    }
    size += TB_WAV_CHUNK_HEADER + layout->data_size + (layout->data_size & 1u);
    if (layout->smpl_size != 0)
    {
        size += TB_WAV_CHUNK_HEADER + layout->smpl_size;
    }
    return size;
}

static void put_chunk_header(unsigned char *bytes, const char *id, uint32_t size)
{
    memcpy(bytes, id, 4);
    tb_wav_put_u32(bytes + 4, size);
}

/* RIFF, `fmt `, and `fact`: everything before the samples. Returns its bytes. */
static size_t put_header(const tb_sound *sound, const wav_layout *layout, uint32_t riff,
                         unsigned char *bytes)
{
    const tb_sound_spec *const spec = &sound->spec;
    const uint32_t bits = (uint32_t)tb_sound_format_size(spec->format) * 8;
    size_t at = 0;

    memcpy(bytes, "RIFF", 4);
    tb_wav_put_u32(bytes + 4, riff);
    memcpy(bytes + 8, "WAVE", 4);
    put_chunk_header(bytes + 12, "fmt ", layout->fmt_size);
    at = 20;
    tb_wav_put_u16(bytes + at, layout->extensible ? TB_WAV_FORMAT_EXTENSIBLE : layout->tag);
    tb_wav_put_u16(bytes + at + 2, (uint16_t)spec->channels);
    tb_wav_put_u32(bytes + at + 4, spec->sample_rate);
    tb_wav_put_u32(bytes + at + 8, spec->sample_rate * (uint32_t)sound->frame_size);
    tb_wav_put_u16(bytes + at + 12, (uint16_t)sound->frame_size);
    tb_wav_put_u16(bytes + at + 14, (uint16_t)bits);
    if (layout->extensible)
    {
        const uint32_t mask = spec->channel_mask != 0
                                  ? spec->channel_mask
                                  : tb_sound_default_channel_mask(spec->channels);
        tb_wav_put_u16(bytes + at + 16, TB_WAV_EXTENSIBLE_EXTRA);
        tb_wav_put_u16(bytes + at + 18, (uint16_t)bits);
        tb_wav_put_u32(bytes + at + 20, mask);
        tb_wav_put_u16(bytes + at + 24, layout->tag);
        memcpy(bytes + at + 26, tb_wav_subformat_tail, sizeof(tb_wav_subformat_tail));
    }
    else if (layout->fmt_size == TB_WAV_FMT_FLOAT_SIZE)
    {
        tb_wav_put_u16(bytes + at + 16, 0);
    }
    at += layout->fmt_size;
    if (layout->has_fact)
    {
        put_chunk_header(bytes + at, "fact", 4);
        tb_wav_put_u32(bytes + at + TB_WAV_CHUNK_HEADER, (uint32_t)sound->frame_count);
        at += TB_WAV_CHUNK_HEADER + 4;
    }
    put_chunk_header(bytes + at, "data", (uint32_t)layout->data_size);
    return at + TB_WAV_CHUNK_HEADER;
}

/* Samples in the file's byte order, little-endian. */
static void put_samples(tb_sound_format format, const unsigned char *source, size_t count,
                        unsigned char *target)
{
    if (format == TB_SOUND_FORMAT_S16)
    {
        for (size_t index = 0; index < count; index++)
        {
            int16_t value = 0;
            memcpy(&value, source + index * 2, sizeof(value));
            tb_wav_put_u16(target + index * 2, (uint16_t)value);
        }
        return;
    }
    if (format == TB_SOUND_FORMAT_S32 || format == TB_SOUND_FORMAT_F32)
    {
        for (size_t index = 0; index < count; index++)
        {
            uint32_t value = 0;
            memcpy(&value, source + index * 4, sizeof(value));
            tb_wav_put_u32(target + index * 4, value);
        }
        return;
    }
    /* U8 and S24 are kept in the file's order. */
    memcpy(target, source, count * tb_sound_format_size(format));
}

static tb_wav_result write_bytes(tb_stream *stream, const void *bytes, size_t length,
                                 tb_wav_error *error)
{
    if (tb_stream_write_all(stream, bytes, length) != TB_STREAM_OK)
    {
        return tb_wav_fail(error, TB_WAV_OUTPUT_FAILED, NULL, "", TB_WAV_NOWHERE,
                           "the stream refused what was written");
    }
    return TB_WAV_OK;
}

static tb_wav_result write_samples(const tb_sound *sound, tb_stream *stream, tb_wav_error *error)
{
    unsigned char scratch[SCRATCH_SIZE] = {0};
    const size_t sample_size = tb_sound_format_size(sound->spec.format);
    /* A whole number of samples of every size, 1, 2, 3 and 4 bytes, fits. */
    const size_t per_part = SCRATCH_SIZE / 12 * 12 / sample_size;
    const size_t total = sound->frame_count * sound->spec.channels;

    for (size_t done = 0; done < total;)
    {
        const size_t part = total - done < per_part ? total - done : per_part;
        put_samples(sound->spec.format, sound->samples + done * sample_size, part, scratch);
        const tb_wav_result result = write_bytes(stream, scratch, part * sample_size, error);
        if (result != TB_WAV_OK)
        {
            return result;
        }
        done += part;
    }
    return TB_WAV_OK;
}

static tb_wav_result write_smpl(const tb_sound *sound, const wav_layout *layout, tb_stream *stream,
                                tb_wav_error *error)
{
    unsigned char bytes[TB_WAV_CHUNK_HEADER + TB_WAV_SMPL_HEADER +
                        TB_WAV_SMPL_LOOP * TB_SOUND_MAX_LOOPS] = {0};
    static const uint32_t kinds[] = {0, 1, 2};
    unsigned char *const header = bytes + TB_WAV_CHUNK_HEADER;

    put_chunk_header(bytes, "smpl", layout->smpl_size);
    tb_wav_put_u32(header + 8, NANOSECONDS_PER_SECOND / sound->spec.sample_rate);
    tb_wav_put_u32(header + 12, MIDI_MIDDLE_C);
    tb_wav_put_u32(header + 28, (uint32_t)sound->loop_count);
    for (size_t index = 0; index < sound->loop_count; index++)
    {
        const tb_sound_loop *const loop = &sound->loops[index];
        unsigned char *const entry = header + TB_WAV_SMPL_HEADER + index * TB_WAV_SMPL_LOOP;
        tb_wav_put_u32(entry, (uint32_t)index);
        tb_wav_put_u32(entry + 4, kinds[loop->kind]);
        tb_wav_put_u32(entry + 8, (uint32_t)loop->start);
        tb_wav_put_u32(entry + 12, (uint32_t)(loop->end - 1));
        tb_wav_put_u32(entry + 20, loop->play_count);
    }
    return write_bytes(stream, bytes, TB_WAV_CHUNK_HEADER + layout->smpl_size, error);
}

tb_wav_result tb_wav_write(const tb_sound *sound, tb_stream *stream, tb_wav_error *error)
{
    unsigned char header[TB_WAV_CHUNK_HEADER * 4 + 4 + TB_WAV_FMT_EXTENSIBLE_SIZE + 4] = {0};
    static const unsigned char padding[1] = {0};

    TB_ASSERT(sound != NULL);
    TB_ASSERT(stream != NULL);
    const wav_layout layout = plan(sound);
    const uint64_t riff = riff_size(&layout);
    if (riff > UINT32_MAX)
    {
        return tb_wav_fail(error, TB_WAV_TOO_LARGE, NULL, "", TB_WAV_NOWHERE,
                           "more than 4 GiB of samples for RIFF");
    }
    const size_t header_size = put_header(sound, &layout, (uint32_t)riff, header);
    tb_wav_result result = write_bytes(stream, header, header_size, error);
    if (result == TB_WAV_OK)
    {
        result = write_samples(sound, stream, error);
    }
    if (result == TB_WAV_OK && (layout.data_size & 1u) != 0)
    {
        result = write_bytes(stream, padding, sizeof(padding), error);
    }
    if (result == TB_WAV_OK && layout.smpl_size != 0)
    {
        result = write_smpl(sound, &layout, stream, error);
    }
    return result;
}
