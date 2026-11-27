#ifndef TB_MIXER_INTERNAL_H
#define TB_MIXER_INTERNAL_H
#include "badger/mixer.h"
#include "badger/platform.h"

/* One slot stays empty to distinguish an empty queue from a full one. */
#define TB_MIXER_COMMAND_SLOTS 257u

struct tb_mixer_clip
{
    tb_audio_format format;
    size_t frames;
    float *samples;
    tb_sound_loop loops[TB_SOUND_MAX_LOOPS];
    size_t loop_count;
    tb_platform_atomic_size uses;
};

typedef struct tb_mixer_cursor
{
    size_t position;
    uint32_t remaining;
    bool looping;
} tb_mixer_cursor;

typedef struct tb_mixer_track
{
    tb_mixer_clip *clip;
    tb_mixer_voice handle;
    tb_mixer_bus bus;
    tb_mixer_cursor cursor;
    tb_sound_loop loop;
    size_t cycle;
    double fraction;
    double pitch;
    double balance[2];
    double gain;
    double target;
    double step;
    size_t fade_frames;
    bool stop_after_fade;
} tb_mixer_track;

typedef struct tb_mixer_slot
{
    size_t reserved;                  /* Written and read only by the producer. */
    tb_platform_atomic_size finished; /* Published by the consumer. */
    tb_mixer_track track;             /* Consumer only. */
} tb_mixer_slot;

typedef struct tb_mixer_command
{
    tb_mixer_voice handle;
    tb_mixer_voice old;
    tb_mixer_clip *clip; /* Non-null for a play; NULL for a fade or stop. */
    tb_mixer_play_desc desc;
    size_t fade_frames;
    bool stop;
} tb_mixer_command;

struct tb_mixer
{
    tb_audio_format format;
    tb_mixer_slot *slots;
    size_t limit;
    size_t serial;
    tb_platform_atomic_u32 volumes[TB_MIXER_BUS_COUNT];
    tb_mixer_command commands[TB_MIXER_COMMAND_SLOTS];
    tb_platform_atomic_size read;
    tb_platform_atomic_size write;
};

void tb_mixer_drain(tb_mixer *mixer);
void tb_mixer_finish(tb_mixer_slot *slot);
#endif
