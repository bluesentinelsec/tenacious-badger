/** @file sound.h
 * @brief A sound in memory: frames of PCM samples at one rate, with their
 * format, their channels, and the loops a sampler plays, made, read,
 * changed, and destroyed.
 *
 * A sound is what a sound effect or a piece of music is once decoded, and
 * what a mixer will play. Files are read into sounds by their own headers,
 * such as badger/wav.h; this header knows no file format.
 *
 * @code
 * const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 48000,
 *                             .channels = 2};
 * tb_sound *beep = NULL;
 * if (tb_sound_create(&spec, 4800, NULL, &beep) != TB_SOUND_OK) { ... }
 * tb_sound_write_frames(beep, 0, 4800, tone, 4800 * 2);  // floats in -1..1
 * tb_sound_fade(beep, 4320, 480, 1.0f, 0.0f);            // the last 10 ms
 * tb_sound_destroy(beep);
 * @endcode
 *
 * **Samples are interleaved**: a frame holds one sample of each channel, in
 * the order of the channel mask's bits, and the frames follow one another.
 * A sample of each format is stored as follows; the integer formats are
 * full scale at -1 and just under +1, and the float format is nominally in
 * -1 to +1, but may go past it:
 *
 * | Format | Bytes | Stored as | Silence |
 * | --- | --- | --- | --- |
 * | U8 | 1 | unsigned, offset by 128 | 128 |
 * | S16 | 2 | `int16_t`, in the machine's byte order | 0 |
 * | S24 | 3 | two's complement, least significant byte first | 0 |
 * | S32 | 4 | `int32_t`, in the machine's byte order | 0 |
 * | F32 | 4 | `float`, in the machine's byte order | 0.0 |
 *
 * **Values.** Functions that read or write samples as floats map an integer
 * sample `s` of `n` bits to `s / 2^(n-1)`, and a float back to the nearest
 * integer of `f * 2^(n-1)`, halves away from zero, clamped to the format's
 * range. A conversion from an integer format to F32 and back is therefore
 * exact for U8, S16 and S24; S32 keeps its top 24 bits through F32. Nothing
 * is dithered: converting to fewer bits rounds.
 *
 * **Ownership.** A sound owns its samples, except one made by
 * tb_sound_create_borrowed(), which reads the caller's samples in place and
 * never writes them: the first function that would write them copies them
 * into memory of its own first. Every sound is released with
 * tb_sound_destroy().
 *
 * **Loops** are ranges of frames a sampler repeats, as a WAV file's `smpl`
 * chunk keeps them for music. Functions that move frames move the loops
 * with them, and drop a loop whose frames they remove.
 *
 * @par Threading
 * A sound is externally synchronized: functions that take a `const
 * tb_sound *` may run together on one sound, from any number of threads,
 * while nothing changes it; a function that changes it runs alone.
 * Different sounds are independent. tb_sound_format_size(),
 * tb_sound_result_name() and tb_sound_default_channel_mask() are
 * thread-safe.
 */
#ifndef TB_SOUND_H
#define TB_SOUND_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_sound_result
{
    TB_SOUND_OK,        /**< Success. */
    TB_SOUND_INVALID,   /**< A format, rate, channel count, length, or loop out of range. */
    TB_SOUND_TOO_LARGE, /**< The samples would need more bytes than `size_t` can count. */
    TB_SOUND_MISMATCH,  /**< Two sounds differ in rate, channels, or format. */
    TB_SOUND_FULL       /**< The sound has #TB_SOUND_MAX_LOOPS loops already. */
} tb_sound_result;

/** @brief How each sample is stored. See the table of the file's comment. */
typedef enum tb_sound_format
{
    TB_SOUND_FORMAT_U8,  /**< Unsigned 8-bit, silence at 128. */
    TB_SOUND_FORMAT_S16, /**< Signed 16-bit. */
    TB_SOUND_FORMAT_S24, /**< Signed 24-bit, packed in 3 bytes. */
    TB_SOUND_FORMAT_S32, /**< Signed 32-bit. */
    TB_SOUND_FORMAT_F32  /**< 32-bit IEEE float. */
} tb_sound_format;

/** @brief The number of formats of #tb_sound_format. */
#define TB_SOUND_FORMAT_COUNT 5

/** @brief The most channels a sound has: 32. */
#define TB_SOUND_MAX_CHANNELS 32u

/** @brief The highest sample rate a sound has, in frames a second: 768 kHz. */
#define TB_SOUND_MAX_RATE 768000u

/** @brief The most loops a sound keeps. */
#define TB_SOUND_MAX_LOOPS 16u

/** @name Speaker positions of a channel mask, as WAVE_FORMAT_EXTENSIBLE numbers them.
 * @{ */
#define TB_SOUND_FRONT_LEFT 0x1u             /**< FL */
#define TB_SOUND_FRONT_RIGHT 0x2u            /**< FR */
#define TB_SOUND_FRONT_CENTER 0x4u           /**< FC */
#define TB_SOUND_LOW_FREQUENCY 0x8u          /**< LFE */
#define TB_SOUND_BACK_LEFT 0x10u             /**< BL */
#define TB_SOUND_BACK_RIGHT 0x20u            /**< BR */
#define TB_SOUND_FRONT_LEFT_OF_CENTER 0x40u  /**< FLC */
#define TB_SOUND_FRONT_RIGHT_OF_CENTER 0x80u /**< FRC */
#define TB_SOUND_BACK_CENTER 0x100u          /**< BC */
#define TB_SOUND_SIDE_LEFT 0x200u            /**< SL */
#define TB_SOUND_SIDE_RIGHT 0x400u           /**< SR */
/** @} */

/** @brief What a sound is made of. */
typedef struct tb_sound_spec
{
    tb_sound_format format; /**< How each sample is stored. */
    uint32_t sample_rate;   /**< Frames a second: 1 to #TB_SOUND_MAX_RATE. */
    uint32_t channels;      /**< Samples a frame: 1 to #TB_SOUND_MAX_CHANNELS. */
    /** Which speaker each channel is for, a bit of `TB_SOUND_FRONT_LEFT` and
     * its kin for each, in the order of the channels; 0 when unknown, which
     * players take as tb_sound_default_channel_mask(). Kept as given. */
    uint32_t channel_mask;
} tb_sound_spec;

/** @brief How a sampler plays a loop. */
typedef enum tb_sound_loop_kind
{
    TB_SOUND_LOOP_FORWARD,   /**< From start to end, then from start again. */
    TB_SOUND_LOOP_PING_PONG, /**< Forward, then backward, then forward. */
    TB_SOUND_LOOP_BACKWARD   /**< From end to start, then from end again. */
} tb_sound_loop_kind;

/** @brief A range of frames a sampler repeats. */
typedef struct tb_sound_loop
{
    size_t start;            /**< The first frame of the loop. */
    size_t end;              /**< One past its last frame; greater than `start`. */
    uint32_t play_count;     /**< Times to play it; 0 for ever. */
    tb_sound_loop_kind kind; /**< How it is played. */
} tb_sound_loop;

/** @brief A sound in memory. Opaque. */
typedef struct tb_sound tb_sound;

/* -------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make a sound of silent frames.
 * @param spec Required; checked.
 * @param frame_count Frames of silence; 0 makes an empty sound.
 * @param allocator Where the sound and its samples live, or NULL for the heap.
 * @param[out] sound Required; the sound on OK, for tb_sound_destroy(); NULL otherwise.
 * @return OK; INVALID for a spec out of range; TOO_LARGE when the samples
 * would not fit in `size_t` bytes.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_create(const tb_sound_spec *spec, size_t frame_count,
                                       const tb_allocator *allocator, tb_sound **sound);

/**
 * @brief Make a sound of a copy of the caller's samples.
 * @param spec Required; checked.
 * @param samples Interleaved samples as the spec says; NULL only at length
 * zero. Copied; need not be aligned.
 * @param length Bytes of `samples`: a whole number of frames.
 * @param allocator Where the sound lives, or NULL for the heap.
 * @param[out] sound Required; the sound on OK; NULL otherwise.
 * @return OK; INVALID for a spec out of range, or a length that is not a
 * whole number of frames.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_create_copy(const tb_sound_spec *spec, const void *samples,
                                            size_t length, const tb_allocator *allocator,
                                            tb_sound **sound);

/**
 * @brief Make a sound that reads the caller's samples in place.
 *
 * The sound never writes to `samples`. The first function that would write
 * them, tb_sound_edit_samples() included, copies them into memory the sound
 * owns, after which the caller's samples are no longer used. Functions that
 * only drop frames from the end, or make new samples, such as a change of
 * format, need no copy.
 * @param spec Required; checked.
 * @param samples Interleaved samples as the spec says; NULL only at length
 * zero. Borrowed: they must stay valid and unchanged until the sound is
 * destroyed or owns a copy. Need not be aligned.
 * @param length Bytes of `samples`: a whole number of frames.
 * @param allocator Where the sound lives, or NULL for the heap.
 * @param[out] sound Required; the sound on OK; NULL otherwise.
 * @return As tb_sound_create_copy().
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_create_borrowed(const tb_sound_spec *spec, const void *samples,
                                                size_t length, const tb_allocator *allocator,
                                                tb_sound **sound);

/**
 * @brief Make a copy of a sound, with its loops, that owns its samples.
 * @param source Required.
 * @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy, for tb_sound_destroy().
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound *tb_sound_duplicate(const tb_sound *source, const tb_allocator *allocator);

/**
 * @brief Make a copy of a range of a sound's frames, with the loops that lie
 * wholly within it, moved to start where the range starts.
 * @param source Required.
 * @param first The first frame to copy; at most the frame count.
 * @param count Frames to copy; `first + count` at most the frame count.
 * @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy, for tb_sound_destroy().
 * @par Errors Running out of memory ends the process. A range past the end
 * breaks the contract.
 */
TB_API tb_sound *tb_sound_copy_range(const tb_sound *source, size_t first, size_t count,
                                     const tb_allocator *allocator);

/** @brief Destroy a sound and release its memory. @param sound Optional; NULL does nothing. */
TB_API void tb_sound_destroy(tb_sound *sound);

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/** @brief The sound's format, rate, channels and channel mask. @param sound Required.
 * @param[out] spec Required. */
TB_API void tb_sound_get_spec(const tb_sound *sound, tb_sound_spec *spec);

/** @brief Frames in the sound. @param sound Required. */
TB_API size_t tb_sound_frame_count(const tb_sound *sound);

/** @brief Bytes of one frame: channels times the size of a sample. @param sound Required. */
TB_API size_t tb_sound_frame_size(const tb_sound *sound);

/** @brief How long the sound plays, in seconds. @param sound Required. */
TB_API double tb_sound_duration(const tb_sound *sound);

/** @brief Whether the sound owns its samples, or reads a caller's. @param sound Required. */
TB_API bool tb_sound_owns_samples(const tb_sound *sound);

/**
 * @brief Borrow the sound's interleaved samples, to read.
 * @param sound Required.
 * @param[out] length Required; bytes of the samples, frame count times frame size.
 * @return The samples, or NULL when there are none. Valid until the sound
 * is changed or destroyed. Aligned for their type when the sound owns them.
 */
TB_API const void *tb_sound_samples(const tb_sound *sound, size_t *length);

/**
 * @brief Borrow the sound's interleaved samples, to change in place.
 *
 * A sound that borrows its samples copies them first.
 * @param sound Required.
 * @param[out] length Required; bytes of the samples.
 * @return The samples, or NULL when there are none. Valid until the sound
 * is next changed by another function, or destroyed.
 * @par Errors Running out of memory for the copy ends the process.
 */
TB_API void *tb_sound_edit_samples(tb_sound *sound, size_t *length);

/**
 * @brief One sample, as a float.
 * @param sound Required. @param frame Less than the frame count.
 * @param channel Less than the channel count.
 * @return The sample, mapped as the file's comment says.
 */
TB_API float tb_sound_get_sample(const tb_sound *sound, size_t frame, uint32_t channel);

/**
 * @brief Set one sample from a float, rounded and clamped to the format.
 * @param sound Required. @param frame Less than the frame count.
 * @param channel Less than the channel count. @param value The sample; NaN is silence.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_set_sample(tb_sound *sound, size_t frame, uint32_t channel, float value);

/**
 * @brief Read frames as interleaved floats, whatever the sound's format.
 * @param sound Required.
 * @param first The first frame; `first + count` at most the frame count.
 * @param count Frames to read.
 * @param[out] samples Required unless `count` is 0; `count` times channels floats.
 * @param capacity Floats `samples` holds; at least `count` times channels.
 */
TB_API void tb_sound_read_frames(const tb_sound *sound, size_t first, size_t count, float *samples,
                                 size_t capacity);

/**
 * @brief Write frames from interleaved floats, rounded and clamped to the format.
 * @param sound Required.
 * @param first The first frame; `first + count` at most the frame count.
 * @param count Frames to write.
 * @param samples Required unless `count` is 0; NaN is written as silence.
 * @param length Floats of `samples`; at least `count` times channels.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_write_frames(tb_sound *sound, size_t first, size_t count, const float *samples,
                                  size_t length);

/**
 * @brief The largest magnitude of any sample, as a float: 0 for silence, 1
 * for an integer sample at its negative full scale.
 * @param sound Required.
 */
TB_API float tb_sound_peak(const tb_sound *sound);

/* -------------------------------------------------------------------------
 * Loops.
 * ------------------------------------------------------------------------- */

/** @brief The number of loops the sound keeps. @param sound Required. */
TB_API size_t tb_sound_loop_count(const tb_sound *sound);

/**
 * @brief One of the sound's loops.
 * @param sound Required. @param index Less than tb_sound_loop_count().
 * @param[out] loop Required.
 */
TB_API void tb_sound_get_loop(const tb_sound *sound, size_t index, tb_sound_loop *loop);

/**
 * @brief Add a loop after those the sound has.
 * @param sound Required. @param loop Required.
 * @return OK; INVALID for a loop that is empty, ends past the last frame, or
 * is of an unknown kind; FULL when the sound has #TB_SOUND_MAX_LOOPS.
 */
TB_API tb_sound_result tb_sound_add_loop(tb_sound *sound, const tb_sound_loop *loop);

/** @brief Remove every loop. @param sound Required. */
TB_API void tb_sound_clear_loops(tb_sound *sound);

/* -------------------------------------------------------------------------
 * Changing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Store the samples in another format, as the file's comment says.
 * @param sound Required. @param format The new format.
 * @return OK; INVALID for an unknown format; TOO_LARGE when the samples in
 * the new format would not fit in `size_t` bytes, leaving the sound as it was.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_convert_format(tb_sound *sound, tb_sound_format format);

/**
 * @brief Give the sound another number of channels.
 *
 * - **One to many:** each new channel is a copy of the one, at full gain, so
 *   a mono sound played in stereo sounds as loud in each speaker.
 * - **Many to one:** the one is the mean of them all (a gain of 1/n each),
 *   so it cannot clip.
 * - **Otherwise:** the channels both counts have are kept, those added are
 *   silent, and those beyond the new count are dropped.
 *
 * The channel mask becomes `channel_mask`.
 * @param sound Required. @param channels 1 to #TB_SOUND_MAX_CHANNELS.
 * @param channel_mask The new mask, or 0 for unknown.
 * @return OK; INVALID for a channel count out of range; TOO_LARGE when the
 * samples would not fit, leaving the sound as it was.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_convert_channels(tb_sound *sound, uint32_t channels,
                                                 uint32_t channel_mask);

/**
 * @brief Play the sound at another rate, keeping its pitch and its length in
 * time, by linear interpolation between neighbouring frames.
 *
 * The new frame count is the old one times `sample_rate`, divided by the old
 * rate and rounded up; frame `i` is taken at `i * old / new` frames of the
 * original. Loops are scaled to the new rate. Linear interpolation is cheap
 * and leaves some aliasing when the rate falls; a filtered resampler is
 * planned.
 * @param sound Required. @param sample_rate 1 to #TB_SOUND_MAX_RATE.
 * @return OK; INVALID for a rate out of range; TOO_LARGE when the new
 * samples would not fit, leaving the sound as it was.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_resample(tb_sound *sound, uint32_t sample_rate);

/**
 * @brief Multiply every sample by a gain, clamping the integer formats.
 * @param sound Required. @param gain Any finite value: 0.5 halves, -1 inverts.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_apply_gain(tb_sound *sound, float gain);

/**
 * @brief Multiply a range of frames by a gain that moves in a straight line
 * from `from_gain`, at the first frame, to `to_gain`, at the last.
 *
 * A fade in is `tb_sound_fade(sound, 0, n, 0.0f, 1.0f)`; a fade out is
 * `tb_sound_fade(sound, frames - n, n, 1.0f, 0.0f)`. A range of one frame
 * takes `to_gain`.
 * @param sound Required.
 * @param first The first frame; `first + count` at most the frame count.
 * @param count Frames to fade.
 * @param from_gain The gain of the first frame.
 * @param to_gain The gain of the last frame.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_fade(tb_sound *sound, size_t first, size_t count, float from_gain,
                          float to_gain);

/**
 * @brief Scale every sample so the largest magnitude is `peak`.
 * @param sound Required. @param peak The magnitude wanted, such as 1.0 or 0.9; above 0.
 * @return The gain applied; 1 for a silent sound, which is left as it is.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API float tb_sound_normalize(tb_sound *sound, float peak);

/**
 * @brief Play the frames backwards. Loops are mirrored with them.
 * @param sound Required.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_reverse(tb_sound *sound);

/**
 * @brief Keep only a range of frames, and the loops wholly within it.
 * @param sound Required.
 * @param first The first frame kept; `first + count` at most the frame count.
 * @param count Frames kept.
 * @par Errors Copying borrowed samples, when out of memory, ends the process.
 */
TB_API void tb_sound_trim(tb_sound *sound, size_t first, size_t count);

/**
 * @brief Add a copy of another sound's frames after the sound's own. Its
 * loops follow, moved, as far as there is room for them.
 * @param sound Required. @param other Required; may be `sound` itself.
 * @return OK; MISMATCH when the two differ in format, rate, or channel
 * count; TOO_LARGE when the samples would not fit, leaving the sound as it was.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_sound_result tb_sound_append(tb_sound *sound, const tb_sound *other);

/* -------------------------------------------------------------------------
 * Names and sizes.
 * ------------------------------------------------------------------------- */

/** @brief Bytes of one sample of a format; 0 for an unknown format. */
TB_API size_t tb_sound_format_size(tb_sound_format format);

/**
 * @brief The channel mask players assume for a count of channels with none:
 * front centre for 1, front left and right for 2, and the layouts of
 * WAVE_FORMAT_EXTENSIBLE's documentation up to 7.1 for 8; 0 past 8.
 */
TB_API uint32_t tb_sound_default_channel_mask(uint32_t channels);

/** @brief A short name of a result, such as "too large". */
TB_API const char *tb_sound_result_name(tb_sound_result result);

#endif
