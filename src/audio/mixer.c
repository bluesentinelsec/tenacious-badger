#include "mixer_internal.h"

#include "badger/assert.h"
#include "badger/engine_settings.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

tb_mixer *tb_mixer_create(const tb_audio_format *format, size_t voice_limit)
{
    TB_ASSERT(format != NULL);
    if (format->sample_rate < TB_AUDIO_DEVICE_MIN_RATE ||
        format->sample_rate > TB_AUDIO_DEVICE_MAX_RATE || format->channels == 0 ||
        format->channels > TB_AUDIO_DEVICE_MAX_CHANNELS || voice_limit == 0 ||
        voice_limit > SIZE_MAX / sizeof(tb_mixer_slot))
    {
        return NULL;
    }
    tb_mixer *const mixer = tb_calloc(1, sizeof(*mixer));
    mixer->format = *format;
    mixer->limit = voice_limit;
    mixer->slots = tb_calloc(voice_limit, sizeof(*mixer->slots));
    for (size_t index = 0; index < TB_MIXER_BUS_COUNT; index++)
    {
        tb_mixer_set_volume(mixer, (tb_mixer_bus)index, 1.0f);
    }
    return mixer;
}

void tb_mixer_destroy(tb_mixer *mixer)
{
    if (mixer == NULL)
    {
        return;
    }
    tb_mixer_drain(mixer);
    for (size_t slot = 0; slot < mixer->limit; slot++)
    {
        tb_mixer_finish(&mixer->slots[slot]);
    }
    tb_free(mixer->slots);
    tb_free(mixer);
}

tb_sound_result tb_mixer_prepare_clip(const tb_mixer *mixer, const tb_sound *sound,
                                      tb_mixer_clip **clip)
{
    TB_ASSERT(mixer != NULL);
    TB_ASSERT(sound != NULL);
    TB_ASSERT(clip != NULL);
    *clip = NULL;
    if (tb_sound_frame_count(sound) == 0)
    {
        return TB_SOUND_INVALID;
    }
    tb_sound *const copy = tb_sound_duplicate(sound, NULL);
    tb_sound_result result = tb_sound_convert_format(copy, TB_SOUND_FORMAT_F32);
    if (result != TB_SOUND_OK)
    {
        goto cleanup;
    }
    result = tb_sound_convert_channels(copy, mixer->format.channels,
                                       tb_sound_default_channel_mask(mixer->format.channels));
    if (result != TB_SOUND_OK)
    {
        goto cleanup;
    }
    result = tb_sound_resample(copy, mixer->format.sample_rate);
    if (result != TB_SOUND_OK)
    {
        goto cleanup;
    }
    size_t byte_count = 0;
    const void *const samples = tb_sound_samples(copy, &byte_count);
    tb_mixer_clip *const prepared = tb_calloc(1, sizeof(*prepared));
    prepared->format = mixer->format;
    prepared->frames = tb_sound_frame_count(copy);
    prepared->samples = tb_calloc(byte_count / sizeof(float), sizeof(float));
    memcpy(prepared->samples, samples, byte_count);
    for (size_t index = 0; index < byte_count / sizeof(float); index++)
    {
        if (!isfinite(prepared->samples[index]))
        {
            prepared->samples[index] = 0.0f;
        }
    }
    prepared->loop_count = tb_sound_loop_count(copy);
    for (size_t index = 0; index < prepared->loop_count; index++)
    {
        tb_sound_get_loop(copy, index, &prepared->loops[index]);
    }
    *clip = prepared;
cleanup:
    tb_sound_destroy(copy);
    return result;
}

bool tb_mixer_clip_destroy(tb_mixer_clip *clip)
{
    if (clip == NULL)
    {
        return true;
    }
    if (tb_platform_atomic_size_load(&clip->uses) != 0)
    {
        return false;
    }
    tb_free(clip->samples);
    tb_free(clip);
    return true;
}

bool tb_mixer_voice_active(const tb_mixer *mixer, tb_mixer_voice voice)
{
    TB_ASSERT(mixer != NULL);
    return voice.serial != 0 && voice.slot < mixer->limit &&
           mixer->slots[voice.slot].reserved == voice.serial &&
           tb_platform_atomic_size_load(&mixer->slots[voice.slot].finished) != voice.serial;
}

/* Prefer a free slot, then the oldest reserved voice. Keep the outgoing
 * voice for the duration of a crossfade. */
static size_t choose_slot(const tb_mixer *mixer, tb_mixer_voice keep)
{
    size_t oldest = mixer->limit;
    for (size_t slot = 0; slot < mixer->limit; slot++)
    {
        const tb_mixer_voice candidate = {slot, mixer->slots[slot].reserved};
        if (!tb_mixer_voice_active(mixer, candidate))
        {
            return slot;
        }
        if (candidate.serial == keep.serial)
        {
            continue;
        }
        if (oldest == mixer->limit || candidate.serial < mixer->slots[oldest].reserved)
        {
            oldest = slot;
        }
    }
    return oldest;
}

/* Only the producer writes at the write cursor. Publish after copying the
 * entire command and taking its clip reference. */
static bool enqueue(tb_mixer *mixer, const tb_mixer_command *command)
{
    const size_t write = tb_platform_atomic_size_load(&mixer->write);
    const size_t next = (write + 1) % TB_MIXER_COMMAND_SLOTS;
    if (next == tb_platform_atomic_size_load(&mixer->read))
    {
        return false;
    }
    if (command->clip != NULL)
    {
        (void)tb_platform_atomic_size_add(&command->clip->uses, 1);
    }
    mixer->commands[write] = *command;
    tb_platform_atomic_size_store(&mixer->write, next);
    return true;
}

static bool valid_gain(float gain)
{
    return gain >= 0.0f && gain <= 1.0f;
}

tb_mixer_voice tb_mixer_crossfade(tb_mixer *mixer, tb_mixer_voice old, tb_mixer_clip *clip,
                                  const tb_mixer_play_desc *desc, size_t frames)
{
    tb_mixer_voice handle = {0};
    TB_ASSERT(mixer != NULL);
    TB_ASSERT(clip != NULL);
    TB_ASSERT(desc != NULL);
    if (clip->format.channels != mixer->format.channels ||
        clip->format.sample_rate != mixer->format.sample_rate ||
        (unsigned)desc->bus >= TB_MIXER_MASTER || !valid_gain(desc->gain) ||
        !(desc->pan >= -1.0f && desc->pan <= 1.0f) ||
        !(desc->pitch > 0.0f && desc->pitch <= 16.0f) ||
        (desc->loop && desc->loop_index != 0 && desc->loop_index >= clip->loop_count) ||
        mixer->serial == SIZE_MAX)
    {
        return handle;
    }
    if (!tb_mixer_voice_active(mixer, old))
    {
        old = (tb_mixer_voice){0};
    }
    const size_t slot = old.serial != 0 && frames == 0 ? old.slot : choose_slot(mixer, old);
    if (slot == mixer->limit)
    {
        return handle;
    }
    const tb_mixer_command play = {.handle = {slot, mixer->serial + 1},
                                   .old = old,
                                   .clip = clip,
                                   .desc = *desc,
                                   .fade_frames = frames};
    if (enqueue(mixer, &play))
    {
        handle = play.handle;
        mixer->slots[slot].reserved = handle.serial;
        mixer->serial = handle.serial;
    }
    return handle;
}

tb_mixer_voice tb_mixer_play(tb_mixer *mixer, tb_mixer_clip *clip, const tb_mixer_play_desc *desc)
{
    return tb_mixer_crossfade(mixer, (tb_mixer_voice){0}, clip, desc, 0);
}

bool tb_mixer_fade(tb_mixer *mixer, tb_mixer_voice voice, float gain, size_t frames)
{
    const tb_mixer_command fade = {.handle = voice, .desc = {.gain = gain}, .fade_frames = frames};
    return tb_mixer_voice_active(mixer, voice) && valid_gain(gain) && enqueue(mixer, &fade);
}

bool tb_mixer_stop(tb_mixer *mixer, tb_mixer_voice voice, size_t frames)
{
    const tb_mixer_command stop = {.handle = voice, .fade_frames = frames, .stop = true};
    return tb_mixer_voice_active(mixer, voice) && enqueue(mixer, &stop);
}

void tb_mixer_set_volume(tb_mixer *mixer, tb_mixer_bus bus, float gain)
{
    uint32_t bits = 0;
    TB_STATIC_ASSERT(sizeof(bits) == sizeof(gain), "Mixer volumes require 32-bit float");
    TB_ASSERT(mixer != NULL);
    TB_ASSERT((unsigned)bus < TB_MIXER_BUS_COUNT);
    TB_ASSERT(valid_gain(gain));
    memcpy(&bits, &gain, sizeof(bits));
    tb_platform_atomic_u32_store(&mixer->volumes[bus], bits);
}

void tb_mixer_apply_settings(tb_mixer *mixer, const tb_engine_settings *settings, bool focused)
{
    TB_ASSERT(settings != NULL);
    const bool mute = !focused && tb_setting_boolean(settings->audio.mute_when_unfocused);
    const float master = mute ? 0.0f : (float)tb_setting_number(settings->audio.master_volume);
    tb_mixer_set_volume(mixer, TB_MIXER_MASTER, master);
    tb_mixer_set_volume(mixer, TB_MIXER_EFFECTS,
                        (float)tb_setting_number(settings->audio.effects_volume));
    tb_mixer_set_volume(mixer, TB_MIXER_MUSIC,
                        (float)tb_setting_number(settings->audio.music_volume));
    tb_mixer_set_volume(mixer, TB_MIXER_AMBIENT,
                        (float)tb_setting_number(settings->audio.ambient_volume));
    tb_mixer_set_volume(mixer, TB_MIXER_VOICE,
                        (float)tb_setting_number(settings->audio.voice_volume));
    tb_mixer_set_volume(mixer, TB_MIXER_INTERFACE,
                        (float)tb_setting_number(settings->audio.interface_volume));
}
