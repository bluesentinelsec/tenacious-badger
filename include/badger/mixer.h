/** @file mixer.h
 * @brief Mix prepared sounds into the float frames of an output device.
 *
 * @par Threading
 * Externally synchronized on the control side: one producer plays voices and
 * changes settings. One consumer calls tb_mixer_render(), either from an audio
 * callback or from an offline renderer. They may run concurrently. Commands
 * take effect at the next render block. The render path neither locks nor
 * allocates. Serialize clip destruction with control threads that might play
 * that clip. Stop the consumer before destroying the mixer.
 */
#ifndef TB_MIXER_H
#define TB_MIXER_H

#include "badger/audio_device.h"
#include "badger/sound.h"

struct tb_engine_settings;

/** @brief Owned mixer. Release with tb_mixer_destroy(). */
typedef struct tb_mixer tb_mixer;
/** @brief Owned sound converted once for a mixer format. Release with tb_mixer_clip_destroy(). */
typedef struct tb_mixer_clip tb_mixer_clip;
/** @brief A voice in one mixer. A zero serial denotes an invalid handle. */
typedef struct tb_mixer_voice
{
    size_t slot;   /**< Mixer slot. */
    size_t serial; /**< Identifies this play even after the slot is reused. */
} tb_mixer_voice;

/** @brief Independent gain groups. Master gain applies to all five groups. */
typedef enum tb_mixer_bus
{
    TB_MIXER_EFFECTS,   /**< Sound effects. */
    TB_MIXER_MUSIC,     /**< Music. */
    TB_MIXER_AMBIENT,   /**< Environmental sound. */
    TB_MIXER_VOICE,     /**< Speech. */
    TB_MIXER_INTERFACE, /**< User interface. */
    TB_MIXER_MASTER,    /**< Combined output; used only to set volume. */
    TB_MIXER_BUS_COUNT  /**< Number of volume controls. */
} tb_mixer_bus;

/** @brief Voice parameters. Initialize with TB_MIXER_PLAY_INIT. */
typedef struct tb_mixer_play_desc
{
    tb_mixer_bus bus;  /**< Destination group. */
    float gain;        /**< Linear amplitude, 0 to 1. */
    float pan;         /**< Stereo balance from -1 (left) through 0 to 1 (right). */
    float pitch;       /**< Source frames per output frame, greater than 0 and at most 16. */
    bool loop;         /**< Repeat a region, or the entire clip if it has no sampler loops. */
    size_t loop_index; /**< Index of the sampler loop to use, zero for the whole clip. */
} tb_mixer_play_desc;

/** @brief Play at original pitch and full gain, centered on the effects bus. */
#define TB_MIXER_PLAY_INIT {TB_MIXER_EFFECTS, 1.0f, 0.0f, 1.0f, false, 0}

/** @brief Create a mixer for an exact device format.
 * @param format Required, with a valid nonzero device rate and channel count.
 * @param voice_limit Maximum simultaneous voices. Must be positive.
 * @return Owned mixer, or NULL for an invalid format or limit.
 */
TB_API tb_mixer *tb_mixer_create(const tb_audio_format *format, size_t voice_limit);
/** @brief Release queued and playing voices. @param mixer Owned mixer, or NULL. */
TB_API void tb_mixer_destroy(tb_mixer *mixer);
/** @brief Copy and convert a sound once into the mixer's sample rate and channel layout.
 * @param mixer Required. @param sound Required. May be edited or destroyed after this call.
 * @param[out] clip Required. Receives an owned clip on success or NULL on failure.
 * @return The sound conversion result. An empty sound returns TB_SOUND_INVALID.
 */
TB_API tb_sound_result tb_mixer_prepare_clip(const tb_mixer *mixer, const tb_sound *sound,
                                             tb_mixer_clip **clip);
/** @brief Release a clip when all queued and playing voices have finished with it.
 * @param clip Owned clip, or NULL.
 * @return True when released. False retains ownership and the caller can retry later.
 */
TB_API bool tb_mixer_clip_destroy(tb_mixer_clip *clip);
/** @brief Play a prepared clip. At the limit, replace the oldest voice.
 * @param mixer Required. @param clip Required clip prepared in the mixer's format.
 * @param desc Required playback parameters, copied on success.
 * @return A voice handle. A zero serial reports invalid parameters or a full command queue.
 */
TB_API tb_mixer_voice tb_mixer_play(tb_mixer *mixer, tb_mixer_clip *clip,
                                    const tb_mixer_play_desc *desc);
/** @brief Start a replacement at zero gain while fading the old voice to silence.
 * @param mixer Required. @param old Voice to fade out, belonging to this mixer.
 * @param clip Required replacement in mixer format. @param desc Required playback parameters.
 * @param frames Output frames in both fades. Zero switches immediately.
 * @return New handle, or a zero serial on failure with the old voice unchanged.
 * Overlapping two voices requires at least two slots. A stale old handle starts a fade-in.
 */
TB_API tb_mixer_voice tb_mixer_crossfade(tb_mixer *mixer, tb_mixer_voice old, tb_mixer_clip *clip,
                                         const tb_mixer_play_desc *desc, size_t frames);
/** @brief Set a voice's gain, optionally over a linear ramp.
 * @param mixer Required. @param voice Handle from this mixer. @param gain Target from 0 to 1.
 * @param frames Duration in output frames; zero applies the change immediately.
 * @return True if queued. False for a stale voice, invalid gain or full queue.
 */
TB_API bool tb_mixer_fade(tb_mixer *mixer, tb_mixer_voice voice, float gain, size_t frames);
/** @brief Stop one voice, optionally after a fade to silence.
 * @param mixer Required. @param voice Handle from this mixer. @param frames Fade duration.
 * @return True if queued. False for a stale voice or full queue.
 */
TB_API bool tb_mixer_stop(tb_mixer *mixer, tb_mixer_voice voice, size_t frames);
/** @brief Check whether a handle still denotes a queued or playing voice.
 * @param mixer Required. @param voice A handle from this mixer.
 * @return True until the voice ends or another play reserves its slot.
 */
TB_API bool tb_mixer_voice_active(const tb_mixer *mixer, tb_mixer_voice voice);
/** @brief Change a volume at the next render block. All volumes start at 1.
 * @param mixer Required. @param bus A gain group or the master. @param gain From 0 to 1.
 */
TB_API void tb_mixer_set_volume(tb_mixer *mixer, tb_mixer_bus bus, float gain);
/** @brief Apply the engine's master and group levels on the control thread.
 * @param mixer Required. @param settings Required engine settings. @param focused Window focus,
 * used with audio.mute_when_unfocused.
 */
TB_API void tb_mixer_apply_settings(tb_mixer *mixer, const struct tb_engine_settings *settings,
                                    bool focused);
/** @brief Audio-device render callback; may also be called offline for deterministic output.
 * @param context Mixer. @param samples Output for frame_count * channels floats; NULL for zero
 * frames.
 * @param frame_count Frames requested. @param channels Must match the mixer.
 * The combined output is clipped to [-1, 1].
 */
TB_API void tb_mixer_render(void *context, float *samples, size_t frame_count, uint32_t channels);

#endif
