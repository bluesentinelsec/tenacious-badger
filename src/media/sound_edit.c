/* Changing sounds: their format, channels and rate, their gain, and which
 * frames they hold. Every value passes through tb_sound_load() and
 * tb_sound_store(), so each operation treats every format alike; a mixer's
 * fast paths for one format are its own business. */
#include "badger/sound.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "sound_internal.h"

#include <stdint.h>
#include <string.h>

/* The largest frame there is: 32 channels of 4 bytes. */
#define MOST_FRAME_BYTES (TB_SOUND_MAX_CHANNELS * 4u)

tb_sound_result tb_sound_convert_format(tb_sound *sound, tb_sound_format format)
{
    tb_sound_spec spec = {0};
    size_t size = 0;

    TB_ASSERT(sound != NULL);
    spec = sound->spec;
    spec.format = format;
    if (tb_sound_format_size(format) == 0)
    {
        return TB_SOUND_INVALID;
    }
    if (format == sound->spec.format)
    {
        return TB_SOUND_OK;
    }
    if (!tb_sound_bytes_of(&spec, sound->frame_count, &size))
    {
        return TB_SOUND_TOO_LARGE;
    }
    const size_t count = sound->frame_count * sound->spec.channels;
    const size_t from_size = tb_sound_format_size(sound->spec.format);
    const size_t to_size = tb_sound_format_size(format);
    unsigned char *const converted = tb_sound_allocate(sound, size);

    for (size_t index = 0; index < count; index++)
    {
        const double value = tb_sound_load(sound->spec.format, sound->samples + index * from_size);
        tb_sound_store(format, converted + index * to_size, value);
    }
    tb_sound_adopt(sound, &spec, converted, size, sound->frame_count);
    return TB_SOUND_OK;
}

/* One frame of `to` channels from one of `from`, by the rules of
 * tb_sound_convert_channels(). */
static void mix_frame(tb_sound_format format, const unsigned char *source, uint32_t from,
                      unsigned char *target, uint32_t to)
{
    const size_t sample_size = tb_sound_format_size(format);

    if (from == 1)
    {
        const double value = tb_sound_load(format, source);
        for (uint32_t channel = 0; channel < to; channel++)
        {
            tb_sound_store(format, target + channel * sample_size, value);
        }
        return;
    }
    if (to == 1)
    {
        double sum = 0.0;
        for (uint32_t channel = 0; channel < from; channel++)
        {
            sum += tb_sound_load(format, source + channel * sample_size);
        }
        tb_sound_store(format, target, sum / (double)from);
        return;
    }
    for (uint32_t channel = 0; channel < to; channel++)
    {
        const double value =
            channel < from ? tb_sound_load(format, source + channel * sample_size) : 0.0;
        tb_sound_store(format, target + channel * sample_size, value);
    }
}

tb_sound_result tb_sound_convert_channels(tb_sound *sound, uint32_t channels, uint32_t channel_mask)
{
    tb_sound_spec spec = {0};
    size_t size = 0;

    TB_ASSERT(sound != NULL);
    spec = sound->spec;
    spec.channels = channels;
    spec.channel_mask = channel_mask;
    if (!tb_sound_spec_is_valid(&spec))
    {
        return TB_SOUND_INVALID;
    }
    if (channels == sound->spec.channels)
    {
        sound->spec.channel_mask = channel_mask;
        return TB_SOUND_OK;
    }
    if (!tb_sound_bytes_of(&spec, sound->frame_count, &size))
    {
        return TB_SOUND_TOO_LARGE;
    }
    const size_t to_frame_size = tb_sound_format_size(spec.format) * channels;
    unsigned char *const converted = tb_sound_allocate(sound, size);

    for (size_t frame = 0; frame < sound->frame_count; frame++)
    {
        mix_frame(spec.format, sound->samples + frame * sound->frame_size, sound->spec.channels,
                  converted + frame * to_frame_size, channels);
    }
    tb_sound_adopt(sound, &spec, converted, size, sound->frame_count);
    return TB_SOUND_OK;
}

/* -------------------------------------------------------------------------
 * Resampling.
 * ------------------------------------------------------------------------- */

/* `value * to / from`, rounded up or down; the caller has checked that
 * `value * to + from` fits. */
static uint64_t scale_frames(uint64_t value, uint32_t from, uint32_t to, bool round_up)
{
    const uint64_t product = value * to;
    if (round_up)
    {
        return (product + from - 1) / from;
    }
    return product / from;
}

/* Move the loops to the new rate. A start rounds down and an end up, so no
 * loop shrinks to nothing, and no end passes the new count, which is the
 * old one scaled and rounded up. */
static void resample_loops(tb_sound *sound, uint32_t from, uint32_t to)
{
    for (size_t index = 0; index < sound->loop_count; index++)
    {
        tb_sound_loop *const loop = &sound->loops[index];
        loop->start = (size_t)scale_frames(loop->start, from, to, false);
        loop->end = (size_t)scale_frames(loop->end, from, to, true);
    }
}

tb_sound_result tb_sound_resample(tb_sound *sound, uint32_t sample_rate)
{
    tb_sound_spec spec = {0};
    size_t size = 0;

    TB_ASSERT(sound != NULL);
    spec = sound->spec;
    spec.sample_rate = sample_rate;
    if (!tb_sound_spec_is_valid(&spec))
    {
        return TB_SOUND_INVALID;
    }
    const uint32_t from = sound->spec.sample_rate;
    if (sample_rate == from)
    {
        return TB_SOUND_OK;
    }
    const uint64_t frames = sound->frame_count;
    if (frames > (UINT64_MAX - from) / sample_rate)
    {
        return TB_SOUND_TOO_LARGE;
    }
    const uint64_t new_frames = scale_frames(frames, from, sample_rate, true);
    if (new_frames > SIZE_MAX || !tb_sound_bytes_of(&spec, (size_t)new_frames, &size))
    {
        return TB_SOUND_TOO_LARGE;
    }
    const size_t sample_size = tb_sound_format_size(spec.format);
    unsigned char *const resampled = tb_sound_allocate(sound, size);
    unsigned char *target = resampled;

    for (uint64_t frame = 0; frame < new_frames; frame++)
    {
        /* Frame `frame` lies at `frame * from / sample_rate` of the original. */
        const uint64_t position = frame * from;
        const size_t before = (size_t)(position / sample_rate);
        const size_t after = before + 1 < sound->frame_count ? before + 1 : before;
        const double fraction = (double)(position % sample_rate) / (double)sample_rate;
        const unsigned char *const left = sound->samples + before * sound->frame_size;
        const unsigned char *const right = sound->samples + after * sound->frame_size;

        for (uint32_t channel = 0; channel < spec.channels; channel++)
        {
            const double a = tb_sound_load(spec.format, left + channel * sample_size);
            const double b = tb_sound_load(spec.format, right + channel * sample_size);
            tb_sound_store(spec.format, target, a + (b - a) * fraction);
            target += sample_size;
        }
    }
    tb_sound_adopt(sound, &spec, resampled, size, (size_t)new_frames);
    resample_loops(sound, from, sample_rate);
    return TB_SOUND_OK;
}

/* -------------------------------------------------------------------------
 * Gain.
 * ------------------------------------------------------------------------- */

/* Multiply the samples of a range of frames by a gain. */
static void scale_frames_by(tb_sound *sound, unsigned char *samples, size_t frame, double gain)
{
    const size_t sample_size = tb_sound_format_size(sound->spec.format);
    unsigned char *sample = samples + frame * sound->frame_size;

    for (uint32_t channel = 0; channel < sound->spec.channels; channel++)
    {
        tb_sound_store(sound->spec.format, sample,
                       tb_sound_load(sound->spec.format, sample) * gain);
        sample += sample_size;
    }
}

void tb_sound_apply_gain(tb_sound *sound, float gain)
{
    TB_ASSERT(sound != NULL);
    if (sound->frame_count == 0)
    {
        return;
    }
    unsigned char *const samples = tb_sound_writable(sound);
    for (size_t frame = 0; frame < sound->frame_count; frame++)
    {
        scale_frames_by(sound, samples, frame, (double)gain);
    }
}

void tb_sound_fade(tb_sound *sound, size_t first, size_t count, float from_gain, float to_gain)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(first <= sound->frame_count && count <= sound->frame_count - first);
    if (count == 0)
    {
        return;
    }
    unsigned char *const samples = tb_sound_writable(sound);
    const double from = (double)from_gain;
    const double step = count > 1 ? ((double)to_gain - from) / (double)(count - 1) : 0.0;

    for (size_t index = 0; index < count; index++)
    {
        const double gain = count > 1 ? from + step * (double)index : (double)to_gain;
        scale_frames_by(sound, samples, first + index, gain);
    }
}

float tb_sound_normalize(tb_sound *sound, float peak)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(peak > 0.0f);
    const float current = tb_sound_peak(sound);
    if (current == 0.0f)
    {
        return 1.0f;
    }
    const float gain = peak / current;
    tb_sound_apply_gain(sound, gain);
    return gain;
}

/* -------------------------------------------------------------------------
 * Which frames.
 * ------------------------------------------------------------------------- */

void tb_sound_reverse(tb_sound *sound)
{
    unsigned char held[MOST_FRAME_BYTES] = {0};

    TB_ASSERT(sound != NULL);
    if (sound->frame_count < 2)
    {
        return;
    }
    unsigned char *const samples = tb_sound_writable(sound);
    const size_t frame_size = sound->frame_size;
    size_t low = 0;
    size_t high = sound->frame_count - 1;

    while (low < high)
    {
        memcpy(held, samples + low * frame_size, frame_size);
        memcpy(samples + low * frame_size, samples + high * frame_size, frame_size);
        memcpy(samples + high * frame_size, held, frame_size);
        low++;
        high--;
    }
    for (size_t index = 0; index < sound->loop_count; index++)
    {
        tb_sound_loop *const loop = &sound->loops[index];
        const size_t start = sound->frame_count - loop->end;
        loop->end = sound->frame_count - loop->start;
        loop->start = start;
    }
}

void tb_sound_trim(tb_sound *sound, size_t first, size_t count)
{
    size_t kept = 0;

    TB_ASSERT(sound != NULL);
    TB_ASSERT(first <= sound->frame_count && count <= sound->frame_count - first);
    if (count != 0 && first != 0)
    {
        unsigned char *const samples = tb_sound_writable(sound);
        memmove(samples, samples + first * sound->frame_size, count * sound->frame_size);
    }
    for (size_t index = 0; index < sound->loop_count; index++)
    {
        tb_sound_loop loop = sound->loops[index];
        if (loop.start >= first && loop.end <= first + count)
        {
            loop.start -= first;
            loop.end -= first;
            sound->loops[kept] = loop;
            kept++;
        }
    }
    sound->loop_count = kept;
    sound->frame_count = count;
    if (count == 0 && sound->borrowed)
    {
        sound->samples = NULL;
        sound->borrowed = false;
    }
}

tb_sound_result tb_sound_append(tb_sound *sound, const tb_sound *other)
{
    size_t frames = 0;
    size_t size = 0;

    TB_ASSERT(sound != NULL);
    TB_ASSERT(other != NULL);
    if (other->spec.format != sound->spec.format ||
        other->spec.sample_rate != sound->spec.sample_rate ||
        other->spec.channels != sound->spec.channels)
    {
        return TB_SOUND_MISMATCH;
    }
    if (!tb_size_add(sound->frame_count, other->frame_count, &frames) ||
        !tb_sound_bytes_of(&sound->spec, frames, &size))
    {
        return TB_SOUND_TOO_LARGE;
    }
    if (other->frame_count == 0)
    {
        return TB_SOUND_OK;
    }
    /* `other` may be `sound`: everything is read from it before it changes. */
    const size_t own_size = sound->frame_count * sound->frame_size;
    const size_t offset = sound->frame_count;
    const size_t other_loops = other->loop_count;
    tb_sound_loop added[TB_SOUND_MAX_LOOPS] = {0};
    unsigned char *const joined = tb_sound_allocate(sound, size);

    memcpy(added, other->loops, other_loops * sizeof(added[0]));
    if (own_size != 0)
    {
        memcpy(joined, sound->samples, own_size);
    }
    memcpy(joined + own_size, other->samples, size - own_size);
    tb_sound_adopt(sound, &sound->spec, joined, size, frames);
    for (size_t index = 0; index < other_loops && sound->loop_count < TB_SOUND_MAX_LOOPS; index++)
    {
        added[index].start += offset;
        added[index].end += offset;
        sound->loops[sound->loop_count] = added[index];
        sound->loop_count++;
    }
    return TB_SOUND_OK;
}
