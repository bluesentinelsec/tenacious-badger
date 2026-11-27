#include "mixer_internal.h"

#include "badger/assert.h"

#include <string.h>

void tb_mixer_finish(tb_mixer_slot *slot)
{
    tb_mixer_track *const track = &slot->track;
    if (track->clip != NULL)
    {
        (void)tb_platform_atomic_size_subtract(&track->clip->uses, 1);
        track->clip = NULL;
        tb_platform_atomic_size_store(&slot->finished, track->handle.serial);
    }
}

static void fade_to(tb_mixer_track *track, double target, size_t frames, bool stop)
{
    track->target = target;
    track->fade_frames = frames;
    track->stop_after_fade = stop;
    if (frames > 0)
    {
        track->step = (target - track->gain) / (double)frames;
    }
    else
    {
        track->gain = target;
    }
}

static void fade_voice(tb_mixer *mixer, tb_mixer_voice voice, float gain, size_t frames, bool stop)
{
    if (voice.serial == 0)
    {
        return;
    }
    tb_mixer_slot *const slot = &mixer->slots[voice.slot];
    if (slot->track.clip == NULL || slot->track.handle.serial != voice.serial)
    {
        return;
    }
    if (stop && frames == 0)
    {
        tb_mixer_finish(slot);
        return;
    }
    fade_to(&slot->track, (double)gain, frames, stop);
}

static void start_voice(tb_mixer *mixer, const tb_mixer_command *play)
{
    tb_mixer_slot *const slot = &mixer->slots[play->handle.slot];
    tb_mixer_track *const track = &slot->track;
    tb_mixer_finish(slot);
    *track = (tb_mixer_track){0};
    track->clip = play->clip;
    track->handle = play->handle;
    track->bus = play->desc.bus;
    track->pitch = (double)play->desc.pitch;
    track->balance[0] = play->desc.pan > 0.0f ? 1.0 - (double)play->desc.pan : 1.0;
    track->balance[1] = play->desc.pan < 0.0f ? 1.0 + (double)play->desc.pan : 1.0;
    fade_to(track, (double)play->desc.gain, play->fade_frames, false);
    if (play->desc.loop)
    {
        track->loop = track->clip->loop_count != 0 ? track->clip->loops[play->desc.loop_index]
                                                   : (tb_sound_loop){.end = track->clip->frames,
                                                                     .kind = TB_SOUND_LOOP_FORWARD};
        const size_t length = track->loop.end - track->loop.start;
        track->cycle =
            track->loop.kind == TB_SOUND_LOOP_PING_PONG && length > 1 ? 2 * (length - 1) : length;
        track->cursor.looping = true;
        track->cursor.remaining = track->loop.play_count;
    }
}

void tb_mixer_drain(tb_mixer *mixer)
{
    size_t read = tb_platform_atomic_size_load(&mixer->read);
    /* Snapshot once. The producer cannot extend this callback's workload. */
    const size_t end = tb_platform_atomic_size_load(&mixer->write);
    while (read != end)
    {
        const tb_mixer_command *const command = &mixer->commands[read];
        if (command->clip != NULL)
        {
            fade_voice(mixer, command->old, 0.0f, command->fade_frames, true);
            start_voice(mixer, command);
        }
        else
        {
            fade_voice(mixer, command->handle, command->desc.gain, command->fade_frames,
                       command->stop);
        }
        read = (read + 1) % TB_MIXER_COMMAND_SLOTS;
    }
    tb_platform_atomic_size_store(&mixer->read, read);
}

/* The cursor travels forwards in time through the intro, loop cycles and
 * tail. Map its position inside each cycle to the corresponding source frame. */
static size_t frame_at(const tb_mixer_track *track, tb_mixer_cursor cursor)
{
    const tb_sound_loop *const loop = &track->loop;
    if (!cursor.looping || cursor.position < loop->start)
    {
        return cursor.position;
    }
    const size_t offset = cursor.position - loop->start;
    if (loop->kind == TB_SOUND_LOOP_BACKWARD)
    {
        return loop->end - 1 - offset;
    }
    if (loop->kind == TB_SOUND_LOOP_PING_PONG && offset >= loop->end - loop->start)
    {
        return loop->start + (track->cycle - offset);
    }
    return cursor.position;
}

static void advance(const tb_mixer_track *track, tb_mixer_cursor *cursor)
{
    cursor->position++;
    if (cursor->looping && cursor->position == track->loop.start + track->cycle)
    {
        if (cursor->remaining == 1)
        {
            cursor->looping = false;
            cursor->position = track->loop.end;
        }
        else
        {
            if (cursor->remaining > 1)
            {
                cursor->remaining--;
            }
            cursor->position = track->loop.start;
        }
    }
}

static bool ended(const tb_mixer_track *track, tb_mixer_cursor cursor)
{
    return !cursor.looping && cursor.position >= track->clip->frames;
}

static void mix_frame(tb_mixer_slot *slot, double *output, uint32_t channels, double volume)
{
    tb_mixer_track *const track = &slot->track;
    tb_mixer_cursor next = track->cursor;
    const size_t first = frame_at(track, track->cursor);
    advance(track, &next);
    const size_t second = ended(track, next) ? first : frame_at(track, next);
    if (track->fade_frames > 0)
    {
        track->fade_frames--;
        track->gain = track->fade_frames == 0 ? track->target : track->gain + track->step;
    }
    const double gain = volume * track->gain;
    for (uint32_t channel = 0; channel < channels; channel++)
    {
        const double left = (double)track->clip->samples[first * channels + channel];
        const double right = (double)track->clip->samples[second * channels + channel];
        const double sample = left + (right - left) * track->fraction;
        const double balance = channels >= 2 && channel < 2 ? track->balance[channel] : 1.0;
        output[channel] += sample * gain * balance;
    }
    if (track->stop_after_fade && track->fade_frames == 0)
    {
        tb_mixer_finish(slot);
        return;
    }
    track->fraction += track->pitch;
    /* Pitch is at most 16, so even tiny sampler loops have bounded cost. */
    while (track->fraction >= 1.0)
    {
        track->fraction -= 1.0;
        track->cursor = next;
        if (ended(track, next))
        {
            tb_mixer_finish(slot);
            return;
        }
        advance(track, &next);
    }
}

void tb_mixer_render(void *context, float *samples, size_t frame_count, uint32_t channels)
{
    tb_mixer *const mixer = context;
    float volumes[TB_MIXER_BUS_COUNT] = {0};
    TB_ASSERT(mixer != NULL);
    TB_ASSERT(channels == mixer->format.channels);
    TB_ASSERT(frame_count == 0 || samples != NULL);
    TB_ASSERT(frame_count <= SIZE_MAX / sizeof(float) / channels);
    tb_mixer_drain(mixer);
    for (size_t index = 0; index < TB_MIXER_BUS_COUNT; index++)
    {
        const uint32_t bits = tb_platform_atomic_u32_load(&mixer->volumes[index]);
        memcpy(&volumes[index], &bits, sizeof(bits));
    }
    for (size_t frame = 0; frame < frame_count; frame++)
    {
        double sum[TB_AUDIO_DEVICE_MAX_CHANNELS] = {0};
        for (size_t slot = 0; slot < mixer->limit; slot++)
        {
            tb_mixer_slot *const voice = &mixer->slots[slot];
            if (voice->track.clip != NULL)
            {
                mix_frame(voice, sum, channels, (double)volumes[voice->track.bus]);
            }
        }
        for (uint32_t channel = 0; channel < channels; channel++)
        {
            double value = sum[channel] * (double)volumes[TB_MIXER_MASTER];
            if (value < -1.0)
            {
                value = -1.0;
            }
            else if (value > 1.0)
            {
                value = 1.0;
            }
            samples[frame * channels + channel] = (float)value;
        }
    }
}
