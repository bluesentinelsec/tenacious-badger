/* Sounds: making, reading, and destroying them, their loops, and the codec
 * of one sample that every function that reads or writes values shares. */
#include "badger/sound.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "sound_internal.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

/* Full scale of each integer format: the magnitude of its most negative value. */
#define U8_SCALE 128.0
#define S16_SCALE 32768.0
#define S24_SCALE 8388608.0
#define S32_SCALE 2147483648.0

#define S24_MIN (-8388608)
#define S24_MAX 8388607

/* -------------------------------------------------------------------------
 * One sample.
 * ------------------------------------------------------------------------- */

static int32_t load_s24(const unsigned char *bytes)
{
    const uint32_t bits =
        (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
    /* Sign-extend from bit 23 without shifting into the sign bit. */
    const int32_t value = (int32_t)(bits & 0x7FFFFFu) - (int32_t)(bits & 0x800000u);
    return value;
}

static void store_s24(unsigned char *bytes, int32_t value)
{
    const uint32_t bits = (uint32_t)value;
    bytes[0] = (unsigned char)(bits & 0xFFu);
    bytes[1] = (unsigned char)((bits >> 8) & 0xFFu);
    bytes[2] = (unsigned char)((bits >> 16) & 0xFFu);
}

/* Every caller has a valid spec, so the format is one of these. */
double tb_sound_load(tb_sound_format format, const unsigned char *bytes)
{
    double result = 0.0;

    switch (format)
    {
    case TB_SOUND_FORMAT_U8:
        result = ((double)bytes[0] - U8_SCALE) / U8_SCALE;
        break;
    case TB_SOUND_FORMAT_S16: {
        int16_t value = 0;
        memcpy(&value, bytes, sizeof(value));
        result = (double)value / S16_SCALE;
        break;
    }
    case TB_SOUND_FORMAT_S24:
        result = (double)load_s24(bytes) / S24_SCALE;
        break;
    case TB_SOUND_FORMAT_S32: {
        int32_t value = 0;
        memcpy(&value, bytes, sizeof(value));
        result = (double)value / S32_SCALE;
        break;
    }
    case TB_SOUND_FORMAT_F32: {
        float value = 0.0f;
        memcpy(&value, bytes, sizeof(value));
        result = (double)value;
        break;
    }
    }
    return result;
}

/* `value` times `scale`, clamped to [minimum, maximum] and rounded half away
 * from zero. Clamping first keeps the conversion to an integer defined. */
static int64_t quantize(double value, double scale, int64_t minimum, int64_t maximum)
{
    const double scaled = value * scale;
    if (scaled != scaled)
    {
        return 0;
    }
    if (scaled >= (double)maximum)
    {
        return maximum;
    }
    if (scaled <= (double)minimum)
    {
        return minimum;
    }
    if (scaled >= 0.0)
    {
        return (int64_t)(scaled + 0.5);
    }
    return -(int64_t)(-scaled + 0.5);
}

void tb_sound_store(tb_sound_format format, unsigned char *bytes, double value)
{
    switch (format)
    {
    case TB_SOUND_FORMAT_U8:
        bytes[0] = (unsigned char)(quantize(value, U8_SCALE, -128, 127) + 128);
        return;
    case TB_SOUND_FORMAT_S16: {
        const int16_t sample = (int16_t)quantize(value, S16_SCALE, INT16_MIN, INT16_MAX);
        memcpy(bytes, &sample, sizeof(sample));
        return;
    }
    case TB_SOUND_FORMAT_S24:
        store_s24(bytes, (int32_t)quantize(value, S24_SCALE, S24_MIN, S24_MAX));
        return;
    case TB_SOUND_FORMAT_S32: {
        const int32_t sample = (int32_t)quantize(value, S32_SCALE, INT32_MIN, INT32_MAX);
        memcpy(bytes, &sample, sizeof(sample));
        return;
    }
    case TB_SOUND_FORMAT_F32: {
        double clamped = value;
        if (clamped != clamped)
        {
            clamped = 0.0;
        }
        else if (clamped > (double)FLT_MAX)
        {
            clamped = FLT_MAX;
        }
        else if (clamped < -(double)FLT_MAX)
        {
            clamped = -FLT_MAX;
        }
        const float sample = (float)clamped;
        memcpy(bytes, &sample, sizeof(sample));
        return;
    }
    }
}

size_t tb_sound_format_size(tb_sound_format format)
{
    switch (format)
    {
    case TB_SOUND_FORMAT_U8:
        return 1;
    case TB_SOUND_FORMAT_S16:
        return 2;
    case TB_SOUND_FORMAT_S24:
        return 3;
    case TB_SOUND_FORMAT_S32:
    case TB_SOUND_FORMAT_F32:
        return 4;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Specs and sizes.
 * ------------------------------------------------------------------------- */

bool tb_sound_spec_is_valid(const tb_sound_spec *spec)
{
    return tb_sound_format_size(spec->format) != 0 && spec->sample_rate >= 1 &&
           spec->sample_rate <= TB_SOUND_MAX_RATE && spec->channels >= 1 &&
           spec->channels <= TB_SOUND_MAX_CHANNELS;
}

bool tb_sound_bytes_of(const tb_sound_spec *spec, size_t frame_count, size_t *bytes)
{
    const size_t frame_size = tb_sound_format_size(spec->format) * spec->channels;
    return tb_size_multiply(frame_count, frame_size, bytes);
}

uint32_t tb_sound_default_channel_mask(uint32_t channels)
{
    static const uint32_t masks[] = {
        0,
        TB_SOUND_FRONT_CENTER,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_FRONT_CENTER,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_BACK_LEFT | TB_SOUND_BACK_RIGHT,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_FRONT_CENTER | TB_SOUND_BACK_LEFT |
            TB_SOUND_BACK_RIGHT,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_FRONT_CENTER |
            TB_SOUND_LOW_FREQUENCY | TB_SOUND_BACK_LEFT | TB_SOUND_BACK_RIGHT,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_FRONT_CENTER |
            TB_SOUND_LOW_FREQUENCY | TB_SOUND_BACK_CENTER | TB_SOUND_SIDE_LEFT |
            TB_SOUND_SIDE_RIGHT,
        TB_SOUND_FRONT_LEFT | TB_SOUND_FRONT_RIGHT | TB_SOUND_FRONT_CENTER |
            TB_SOUND_LOW_FREQUENCY | TB_SOUND_BACK_LEFT | TB_SOUND_BACK_RIGHT | TB_SOUND_SIDE_LEFT |
            TB_SOUND_SIDE_RIGHT,
    };
    if (channels >= sizeof(masks) / sizeof(masks[0]))
    {
        return 0;
    }
    return masks[channels];
}

const char *tb_sound_result_name(tb_sound_result result)
{
    switch (result)
    {
    case TB_SOUND_OK:
        return "ok";
    case TB_SOUND_INVALID:
        return "invalid argument";
    case TB_SOUND_TOO_LARGE:
        return "too large";
    case TB_SOUND_MISMATCH:
        return "sounds differ";
    case TB_SOUND_FULL:
        return "too many loops";
    }
    return "unknown result";
}

/* -------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

unsigned char *tb_sound_allocate(const tb_sound *sound, size_t size)
{
    if (size == 0)
    {
        return NULL;
    }
    return tb_allocator_alloc(&sound->allocator, size, TB_SOUND_ALIGNMENT);
}

void tb_sound_adopt(tb_sound *sound, const tb_sound_spec *spec, unsigned char *samples, size_t size,
                    size_t frame_count)
{
    if (sound->owned != NULL)
    {
        tb_allocator_free(&sound->allocator, sound->owned, sound->owned_size, TB_SOUND_ALIGNMENT);
    }
    sound->spec = *spec;
    sound->frame_size = tb_sound_format_size(spec->format) * spec->channels;
    sound->frame_count = frame_count;
    sound->owned = samples;
    sound->owned_size = samples != NULL ? size : 0;
    sound->samples = samples;
    sound->borrowed = false;
}

/* An empty sound of a valid spec, with no samples. */
static tb_sound *make_shell(const tb_sound_spec *spec, const tb_allocator *allocator)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_sound *const sound = TB_ALLOCATOR_NEW(&resolved, tb_sound);

    sound->allocator = resolved;
    sound->spec = *spec;
    sound->frame_size = tb_sound_format_size(spec->format) * spec->channels;
    return sound;
}

static void release_shell(tb_sound *sound)
{
    const tb_allocator allocator = sound->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_sound, sound);
}

tb_sound *tb_sound_try_create(const tb_sound_spec *spec, size_t frame_count,
                              const tb_allocator *allocator)
{
    size_t size = 0;
    unsigned char *samples = NULL;

    TB_ASSERT(tb_sound_spec_is_valid(spec));
    if (!tb_sound_bytes_of(spec, frame_count, &size))
    {
        return NULL;
    }
    tb_sound *const sound = make_shell(spec, allocator);
    if (size != 0)
    {
        samples = tb_allocator_try_alloc(&sound->allocator, size, TB_SOUND_ALIGNMENT);
        if (samples == NULL)
        {
            release_shell(sound);
            return NULL;
        }
    }
    tb_sound_adopt(sound, spec, samples, size, frame_count);
    return sound;
}

/* Fill frames with the format's silence. */
static void fill_silence(tb_sound_format format, unsigned char *bytes, size_t size)
{
    if (format == TB_SOUND_FORMAT_U8)
    {
        memset(bytes, 128, size);
        return;
    }
    memset(bytes, 0, size);
}

tb_sound_result tb_sound_create(const tb_sound_spec *spec, size_t frame_count,
                                const tb_allocator *allocator, tb_sound **sound)
{
    size_t size = 0;

    TB_ASSERT(spec != NULL);
    TB_ASSERT(sound != NULL);
    *sound = NULL;
    if (!tb_sound_spec_is_valid(spec))
    {
        return TB_SOUND_INVALID;
    }
    if (!tb_sound_bytes_of(spec, frame_count, &size))
    {
        return TB_SOUND_TOO_LARGE;
    }
    tb_sound *const made = make_shell(spec, allocator);
    unsigned char *const samples = tb_sound_allocate(made, size);
    if (samples != NULL)
    {
        fill_silence(spec->format, samples, size);
    }
    tb_sound_adopt(made, spec, samples, size, frame_count);
    *sound = made;
    return TB_SOUND_OK;
}

/* The frames of `length` bytes of a spec, or false when it is not whole frames. */
static bool frames_of_length(const tb_sound_spec *spec, size_t length, size_t *frames)
{
    const size_t frame_size = tb_sound_format_size(spec->format) * spec->channels;
    if (length % frame_size != 0)
    {
        return false;
    }
    *frames = length / frame_size;
    return true;
}

tb_sound_result tb_sound_create_copy(const tb_sound_spec *spec, const void *samples, size_t length,
                                     const tb_allocator *allocator, tb_sound **sound)
{
    size_t frames = 0;

    TB_ASSERT(spec != NULL);
    TB_ASSERT(sound != NULL);
    TB_ASSERT(samples != NULL || length == 0);
    *sound = NULL;
    if (!tb_sound_spec_is_valid(spec) || !frames_of_length(spec, length, &frames))
    {
        return TB_SOUND_INVALID;
    }
    tb_sound *const made = make_shell(spec, allocator);
    unsigned char *const copy = tb_sound_allocate(made, length);
    if (copy != NULL)
    {
        memcpy(copy, samples, length);
    }
    tb_sound_adopt(made, spec, copy, length, frames);
    *sound = made;
    return TB_SOUND_OK;
}

tb_sound_result tb_sound_create_borrowed(const tb_sound_spec *spec, const void *samples,
                                         size_t length, const tb_allocator *allocator,
                                         tb_sound **sound)
{
    size_t frames = 0;

    TB_ASSERT(spec != NULL);
    TB_ASSERT(sound != NULL);
    TB_ASSERT(samples != NULL || length == 0);
    *sound = NULL;
    if (!tb_sound_spec_is_valid(spec) || !frames_of_length(spec, length, &frames))
    {
        return TB_SOUND_INVALID;
    }
    tb_sound *const made = make_shell(spec, allocator);
    made->frame_count = frames;
    if (length != 0)
    {
        made->samples = samples;
        made->borrowed = true;
    }
    *sound = made;
    return TB_SOUND_OK;
}

tb_sound *tb_sound_copy_range(const tb_sound *source, size_t first, size_t count,
                              const tb_allocator *allocator)
{
    TB_ASSERT(source != NULL);
    TB_ASSERT(first <= source->frame_count && count <= source->frame_count - first);
    const size_t size = count * source->frame_size;
    tb_sound *const copy = make_shell(&source->spec, allocator);
    unsigned char *const samples = tb_sound_allocate(copy, size);

    if (samples != NULL)
    {
        memcpy(samples, source->samples + first * source->frame_size, size);
    }
    tb_sound_adopt(copy, &source->spec, samples, size, count);
    for (size_t index = 0; index < source->loop_count; index++)
    {
        const tb_sound_loop *const loop = &source->loops[index];
        if (loop->start >= first && loop->end <= first + count)
        {
            copy->loops[copy->loop_count] = *loop;
            copy->loops[copy->loop_count].start -= first;
            copy->loops[copy->loop_count].end -= first;
            copy->loop_count++;
        }
    }
    return copy;
}

tb_sound *tb_sound_duplicate(const tb_sound *source, const tb_allocator *allocator)
{
    TB_ASSERT(source != NULL);
    return tb_sound_copy_range(source, 0, source->frame_count, allocator);
}

void tb_sound_destroy(tb_sound *sound)
{
    if (sound == NULL)
    {
        return;
    }
    if (sound->owned != NULL)
    {
        tb_allocator_free(&sound->allocator, sound->owned, sound->owned_size, TB_SOUND_ALIGNMENT);
    }
    release_shell(sound);
}

unsigned char *tb_sound_writable(tb_sound *sound)
{
    if (sound->borrowed)
    {
        const size_t size = sound->frame_count * sound->frame_size;
        unsigned char *const copy = tb_sound_allocate(sound, size);
        memcpy(copy, sound->samples, size);
        tb_sound_adopt(sound, &sound->spec, copy, size, sound->frame_count);
    }
    return sound->owned;
}

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

void tb_sound_get_spec(const tb_sound *sound, tb_sound_spec *spec)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(spec != NULL);
    *spec = sound->spec;
}

size_t tb_sound_frame_count(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    return sound->frame_count;
}

size_t tb_sound_frame_size(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    return sound->frame_size;
}

double tb_sound_duration(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    return (double)sound->frame_count / (double)sound->spec.sample_rate;
}

bool tb_sound_owns_samples(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    return !sound->borrowed;
}

const void *tb_sound_samples(const tb_sound *sound, size_t *length)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(length != NULL);
    *length = sound->frame_count * sound->frame_size;
    return sound->samples;
}

void *tb_sound_edit_samples(tb_sound *sound, size_t *length)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(length != NULL);
    *length = sound->frame_count * sound->frame_size;
    return tb_sound_writable(sound);
}

static size_t sample_offset(const tb_sound *sound, size_t frame, uint32_t channel)
{
    TB_ASSERT(frame < sound->frame_count);
    TB_ASSERT(channel < sound->spec.channels);
    return frame * sound->frame_size + channel * tb_sound_format_size(sound->spec.format);
}

float tb_sound_get_sample(const tb_sound *sound, size_t frame, uint32_t channel)
{
    TB_ASSERT(sound != NULL);
    const size_t offset = sample_offset(sound, frame, channel);
    return (float)tb_sound_load(sound->spec.format, sound->samples + offset);
}

void tb_sound_set_sample(tb_sound *sound, size_t frame, uint32_t channel, float value)
{
    TB_ASSERT(sound != NULL);
    const size_t offset = sample_offset(sound, frame, channel);
    unsigned char *const samples = tb_sound_writable(sound);
    tb_sound_store(sound->spec.format, samples + offset, (double)value);
}

/* Check a range of frames and a buffer of floats for it; the floats it needs. */
static size_t check_frames(const tb_sound *sound, size_t first, size_t count, const float *floats,
                           size_t capacity)
{
    TB_ASSERT(first <= sound->frame_count && count <= sound->frame_count - first);
    const size_t needed = count * sound->spec.channels;
    TB_ASSERT(capacity >= needed);
    TB_ASSERT(floats != NULL || count == 0);
    (void)floats;
    (void)capacity;
    return needed;
}

void tb_sound_read_frames(const tb_sound *sound, size_t first, size_t count, float *samples,
                          size_t capacity)
{
    TB_ASSERT(sound != NULL);
    const size_t needed = check_frames(sound, first, count, samples, capacity);
    const size_t sample_size = tb_sound_format_size(sound->spec.format);
    const unsigned char *source = needed != 0 ? sound->samples + first * sound->frame_size : NULL;

    for (size_t index = 0; index < needed; index++)
    {
        samples[index] = (float)tb_sound_load(sound->spec.format, source);
        source += sample_size;
    }
}

void tb_sound_write_frames(tb_sound *sound, size_t first, size_t count, const float *samples,
                           size_t length)
{
    TB_ASSERT(sound != NULL);
    const size_t needed = check_frames(sound, first, count, samples, length);
    if (needed == 0)
    {
        return;
    }
    const size_t sample_size = tb_sound_format_size(sound->spec.format);
    unsigned char *target = tb_sound_writable(sound) + first * sound->frame_size;

    for (size_t index = 0; index < needed; index++)
    {
        tb_sound_store(sound->spec.format, target, (double)samples[index]);
        target += sample_size;
    }
}

float tb_sound_peak(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    const size_t count = sound->frame_count * sound->spec.channels;
    const size_t sample_size = tb_sound_format_size(sound->spec.format);
    double peak = 0.0;

    for (size_t index = 0; index < count; index++)
    {
        const double value =
            tb_sound_load(sound->spec.format, sound->samples + index * sample_size);
        const double magnitude = value < 0.0 ? -value : value;
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    return (float)peak;
}

/* -------------------------------------------------------------------------
 * Loops.
 * ------------------------------------------------------------------------- */

size_t tb_sound_loop_count(const tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    return sound->loop_count;
}

void tb_sound_get_loop(const tb_sound *sound, size_t index, tb_sound_loop *loop)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(loop != NULL);
    TB_ASSERT(index < sound->loop_count);
    *loop = sound->loops[index];
}

tb_sound_result tb_sound_add_loop(tb_sound *sound, const tb_sound_loop *loop)
{
    TB_ASSERT(sound != NULL);
    TB_ASSERT(loop != NULL);
    const bool known_kind = loop->kind == TB_SOUND_LOOP_FORWARD ||
                            loop->kind == TB_SOUND_LOOP_PING_PONG ||
                            loop->kind == TB_SOUND_LOOP_BACKWARD;
    if (loop->start >= loop->end || loop->end > sound->frame_count || !known_kind)
    {
        return TB_SOUND_INVALID;
    }
    if (sound->loop_count == TB_SOUND_MAX_LOOPS)
    {
        return TB_SOUND_FULL;
    }
    sound->loops[sound->loop_count] = *loop;
    sound->loop_count++;
    return TB_SOUND_OK;
}

void tb_sound_clear_loops(tb_sound *sound)
{
    TB_ASSERT(sound != NULL);
    sound->loop_count = 0;
}
