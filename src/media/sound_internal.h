/* What the sound's sources share with one another and with the WAV reader:
 * the sound itself, the codec of one sample, and the replacing of samples. */
#ifndef TB_SOUND_INTERNAL_H
#define TB_SOUND_INTERNAL_H

#include "badger/sound.h"

#include <stdbool.h>
#include <stddef.h>

/* Owned samples are aligned for every format, and for vector loads later. */
#define TB_SOUND_ALIGNMENT 16

struct tb_sound
{
    tb_allocator allocator;
    tb_sound_spec spec;
    size_t frame_count;
    size_t frame_size;
    /* What is read: the owned samples, or the caller's; NULL when empty. */
    const unsigned char *samples;
    /* The samples the sound owns, of `owned_size` bytes; NULL when it owns none. */
    unsigned char *owned;
    size_t owned_size;
    bool borrowed;
    size_t loop_count;
    tb_sound_loop loops[TB_SOUND_MAX_LOOPS];
};

/* Whether a spec is in range. */
bool tb_sound_spec_is_valid(const tb_sound_spec *spec);

/* Bytes of `frame_count` frames of a valid spec; false when they overflow. */
bool tb_sound_bytes_of(const tb_sound_spec *spec, size_t frame_count, size_t *bytes);

/* A sample as a value, integers scaled to -1..1. */
double tb_sound_load(tb_sound_format format, const unsigned char *bytes);

/* Store a value as a sample: rounded half away from zero and clamped for
 * the integer formats, clamped to the range of a float for F32; NaN is
 * silence. */
void tb_sound_store(tb_sound_format format, unsigned char *bytes, double value);

/* A sound of a valid spec with `frame_count` uninitialized frames, or NULL
 * when the memory is not there. */
tb_sound *tb_sound_try_create(const tb_sound_spec *spec, size_t frame_count,
                              const tb_allocator *allocator);

/* The sound's samples, owned by it, to write: borrowed samples are copied first. */
unsigned char *tb_sound_writable(tb_sound *sound);

/* New samples of `size` bytes from the sound's allocator, which the sound
 * adopts with tb_sound_adopt(). */
unsigned char *tb_sound_allocate(const tb_sound *sound, size_t size);

/* Make `samples`, of `size` bytes from tb_sound_allocate() and of
 * `frame_count` frames of `spec`, the sound's own, releasing what it owned. */
void tb_sound_adopt(tb_sound *sound, const tb_sound_spec *spec, unsigned char *samples, size_t size,
                    size_t frame_count);

#endif
