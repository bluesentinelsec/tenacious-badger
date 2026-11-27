/** @file audio_device.h
 * @brief Audio output devices: the speakers and headphones a game plays
 * through, listed, opened, and fed samples by a function the device calls
 * on its own thread.
 *
 * An audio system is the connection to the platform's audio: WASAPI on
 * Windows; PipeWire, or ALSA where PipeWire is not running, on Linux and the
 * Steam Deck; and Core Audio on macOS. It lists the output devices and opens
 * a device for playback. An open device calls a **render function** of the
 * caller's, on a thread of the device's own, each time it needs samples: a
 * pull model, as SDL's audio callback and Core Audio's render callback are.
 *
 * @code
 * static void render(void *context, float *samples, size_t frame_count, uint32_t channels)
 * {
 *     mix(context, samples, frame_count * channels); // no locks, no allocation
 * }
 *
 * tb_audio_system *audio = NULL;
 * tb_audio_device *device = NULL;
 * tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
 * const tb_audio_device_desc desc = {.format = {.sample_rate = 48000, .channels = 2},
 *                                    .render = render, .context = mixer};
 * if (tb_audio_system_open(&audio, &error) != TB_AUDIO_DEVICE_OK ||
 *     tb_audio_device_open(audio, &desc, &device, &error) != TB_AUDIO_DEVICE_OK)
 * {
 *     report(error.message);
 *     tb_audio_system_close(audio);
 *     return; // play on in silence
 * }
 * tb_audio_device_start(device);
 * // Once a frame, on the main thread:
 * tb_audio_device_event event = {0};
 * while (tb_audio_device_next_event(device, &event)) { ... }
 * @endcode
 *
 * **Samples.** A device is always fed 32-bit floats, interleaved: a frame
 * holds one sample of each channel, in the order of the format's channel
 * mask (front left, then front right, for stereo), as badger/sound.h lays
 * them out. Full scale is -1 to 1; a sample beyond it is clipped by the
 * system. The rate and channels are those the caller asks for, or the
 * device's own when it asks for none, and they never change while the
 * device is open: when the system's device runs at another rate or has
 * other speakers, the system converts.
 *
 * **The default device.** A device opened without an identifier plays on the
 * system's default output. On backends with `follows_default`, when the player picks another
 * default or unplugs the headphones, the device moves to the new default and
 * reports #TB_AUDIO_DEVICE_EVENT_CHANGED. A device opened by identifier
 * plays there until that device goes away, then moves to the default. While
 * there is no device at all, the device reports #TB_AUDIO_DEVICE_EVENT_LOST,
 * its samples go nowhere, and it comes back when a device does. None of this
 * is a failure the caller handles: a game keeps its device open.
 *
 * **What is not there is a capability.** A machine without audio, a CI
 * runner, or a system whose audio library is missing gets
 * #TB_AUDIO_DEVICE_UNAVAILABLE, and a game plays on without sound.
 *
 * See `docs/audio.md`.
 *
 * @par Threading
 * Externally synchronized: the caller serializes the calls on a system and
 * on its devices, usually by making them all from the main thread. A
 * device's render function runs on the device's own thread, and is the one
 * exception; its contract is #tb_audio_render_function's.
 * tb_audio_device_result_name() is thread-safe.
 */
#ifndef TB_AUDIO_DEVICE_H
#define TB_AUDIO_DEVICE_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The connection to the platform's audio. Opened by
 * tb_audio_system_open(); owned by the caller; closed by
 * tb_audio_system_close(), after its devices. */
typedef struct tb_audio_system tb_audio_system;

/** @brief An output device, open for playback. Opened by
 * tb_audio_device_open(); owned by the caller; closed by
 * tb_audio_device_close(). */
typedef struct tb_audio_device tb_audio_device;

/** @brief The most channels a device is opened with: 7.1. */
#define TB_AUDIO_DEVICE_MAX_CHANNELS 8u

/** @brief The lowest and highest sample rates a device is opened at, in
 * frames a second. */
#define TB_AUDIO_DEVICE_MIN_RATE 8000u
#define TB_AUDIO_DEVICE_MAX_RATE 384000u /**< @copydoc TB_AUDIO_DEVICE_MIN_RATE */

/** @brief Bytes of a device's identifier, its NUL included. */
#define TB_AUDIO_DEVICE_ID_CAPACITY 256

/** @brief Bytes of a device's name, its NUL included. */
#define TB_AUDIO_DEVICE_NAME_CAPACITY 128

/** @brief Bytes of a backend's name, its NUL included. */
#define TB_AUDIO_BACKEND_NAME_CAPACITY 32

/** @brief Bytes of an error's message, its NUL included. */
#define TB_AUDIO_DEVICE_MESSAGE_CAPACITY 256

/** @brief What a call came to. */
typedef enum tb_audio_device_result
{
    TB_AUDIO_DEVICE_OK, /**< Done. */
    /** No audio: the system's library is missing, its audio service is not
     * running, or it has no output device. A capability, not a fault: a
     * game plays on in silence. */
    TB_AUDIO_DEVICE_UNAVAILABLE,
    /** No output device has the identifier asked for. */
    TB_AUDIO_DEVICE_NOT_FOUND,
    /** The system refused the format or the device; the message says how. */
    TB_AUDIO_DEVICE_SYSTEM_ERROR
} tb_audio_device_result;

/** @brief Why a call failed. */
typedef struct tb_audio_device_error
{
    tb_audio_device_result result; /**< As the function returned. */
    /** What went wrong, in UTF-8: "WASAPI: no output device". */
    char message[TB_AUDIO_DEVICE_MESSAGE_CAPACITY];
} tb_audio_device_error;

/** @brief An error before a call. */
#define TB_AUDIO_DEVICE_ERROR_INIT {TB_AUDIO_DEVICE_OK, {0}}

/** @brief What a system's backend is and can do: its capabilities. */
typedef struct tb_audio_system_info
{
    /** Its name: "WASAPI", "PipeWire", "ALSA", "Core Audio", or a test's. */
    char backend[TB_AUDIO_BACKEND_NAME_CAPACITY];
    /** A device on the default output follows the default when it changes,
     * and one whose device goes away moves to the default. */
    bool follows_default;
    /** It reports #TB_AUDIO_DEVICE_EVENT_CHANGED and
     * #TB_AUDIO_DEVICE_EVENT_LOST. A backend whose system moves streams by
     * itself, as PipeWire's session manager does, may not say when. */
    bool reports_changes;
    /** tb_audio_system_list_devices() marks the default device. */
    bool knows_default;
} tb_audio_system_info;

/** @brief An output device, as tb_audio_system_list_devices() lists it. */
typedef struct tb_audio_device_info
{
    /** What the system calls it, to open it by and to keep in the
     * `audio.output_device` setting: stable while the device stays plugged
     * in, and usually across restarts. UTF-8. */
    char id[TB_AUDIO_DEVICE_ID_CAPACITY];
    /** Its name for a player, in UTF-8: "Speakers (Realtek(R) Audio)". Cut
     * at a character when too long. */
    char name[TB_AUDIO_DEVICE_NAME_CAPACITY];
    /** It is the system's default output. */
    bool is_default;
    /** The rate it runs at, in frames a second, where the system says; else 0. */
    uint32_t sample_rate;
    /** Its channels, where the system says; else 0. */
    uint32_t channels;
} tb_audio_device_info;

/** @brief The samples a device is fed: 32-bit floats, interleaved, at a
 * rate, in a number of channels. */
typedef struct tb_audio_format
{
    /** Frames a second, from #TB_AUDIO_DEVICE_MIN_RATE to
     * #TB_AUDIO_DEVICE_MAX_RATE; 0 asks for the device's own. */
    uint32_t sample_rate;
    /** Samples a frame, from 1 to #TB_AUDIO_DEVICE_MAX_CHANNELS; 0 asks for
     * the device's own, at most #TB_AUDIO_DEVICE_MAX_CHANNELS. */
    uint32_t channels;
} tb_audio_format;

/**
 * @brief Fill a device's samples: what an open device calls, on its own
 * thread, each time it needs more.
 *
 * It is called only while the device is started, and never twice at once.
 * The device's thread is a real-time one, as the system's audio is, so the
 * function must return quickly and must not wait: no locks, no
 * allocation, no files, no logging, nothing that may block. It reads what
 * the game sends it through lock-free queues and atomics.
 *
 * @param context The `context` of the device's description.
 * @param samples `frame_count * channels` floats, silent when the function
 * is called, to write the frames into; valid only during the call.
 * @param frame_count Frames to fill: as many as the device needs now, which
 * can vary from call to call.
 * @param channels The device's channels, as tb_audio_device_get_format() says.
 */
typedef void tb_audio_render_function(void *context, float *samples, size_t frame_count,
                                      uint32_t channels);

/** @brief The device to open, and how. */
typedef struct tb_audio_device_desc
{
    /** The identifier of a device of tb_audio_system_list_devices(); NULL
     * or "" for the default output, which the device then follows.
     * Borrowed for the call. */
    const char *device_id;
    /** The rate and channels to feed it; zero asks for the device's own. */
    tb_audio_format format;
    /** Frames the device asks for at a time, as a hint: fewer for less
     * latency, more for fewer wake-ups. 0 asks for the backend's default,
     * about 10 ms. The system rounds it to what it can do; the latency says
     * what it chose. */
    uint32_t period_frames;
    /** The stream's name in the system's mixer, as the volume mixer of
     * Windows and PipeWire's tools show it: UTF-8, borrowed for the call;
     * NULL for "badger". */
    const char *stream_name;
    /** The render function; required. */
    tb_audio_render_function *render;
    /** Passed to `render`; may be NULL. */
    void *context;
} tb_audio_device_desc;

/** @brief How far ahead of the speakers a device's samples are. */
typedef struct tb_audio_device_latency
{
    /** Frames the device asks for at a time, as the system chose them. */
    uint32_t period_frames;
    /** Estimated device buffer size in frames at the stream's sample rate. */
    uint32_t buffer_frames;
    /** Seconds from a sample written to it being heard, as far as the
     * system says: the buffer, and the device's own latency where known. */
    double seconds;
} tb_audio_device_latency;

/** @brief What happened to an open device. */
typedef enum tb_audio_device_event_type
{
    /** It plays on another device now: a new default, or the default after
     * its own went away. The event names it. */
    TB_AUDIO_DEVICE_EVENT_CHANGED,
    /** There is no device to play on; its samples go nowhere until one
     * comes, when it reports #TB_AUDIO_DEVICE_EVENT_CHANGED. */
    TB_AUDIO_DEVICE_EVENT_LOST
} tb_audio_device_event_type;

/** @brief Something that happened to a device, in the order it happened. */
typedef struct tb_audio_device_event
{
    tb_audio_device_event_type type; /**< What happened. */
    /** The device it plays on now, for #TB_AUDIO_DEVICE_EVENT_CHANGED, as
     * the system names it; "" where it does not say, and for a loss. */
    char device_name[TB_AUDIO_DEVICE_NAME_CAPACITY];
} tb_audio_device_event;

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

/**
 * @brief Connect to the platform's audio.
 * @param[out] system Receives the system, or NULL on failure; required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_AUDIO_DEVICE_OK, #TB_AUDIO_DEVICE_UNAVAILABLE or
 * #TB_AUDIO_DEVICE_SYSTEM_ERROR.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_audio_device_result tb_audio_system_open(tb_audio_system **system,
                                                   tb_audio_device_error *error);

/** @brief Close an audio system. Its devices are closed first, by the
 * caller. @param system The system; NULL is ignored. */
TB_API void tb_audio_system_close(tb_audio_system *system);

/** @brief What the system's backend is and can do.
 * @param system Required non-null. @param[out] info Required non-null. */
TB_API void tb_audio_system_get_info(const tb_audio_system *system, tb_audio_system_info *info);

/**
 * @brief List the output devices, as the system has them now.
 * @param system Required non-null.
 * @param[out] devices Receives up to `capacity` devices, in the system's
 * order; NULL only with a capacity of 0, to count them.
 * @param capacity Devices `devices` holds.
 * @return How many devices there are, which may be more than `capacity`;
 * 0 when there are none, or when the system cannot list them.
 */
TB_API size_t tb_audio_system_list_devices(tb_audio_system *system, tb_audio_device_info *devices,
                                           size_t capacity);

/* ---------------------------------------------------------------------------
 * Devices.
 * ------------------------------------------------------------------------- */

/**
 * @brief Open an output device for playback, stopped: it plays silence
 * until tb_audio_device_start().
 * @param system Required non-null; outlives the device.
 * @param desc Required non-null; a rate or channels out of range, or no
 * render function, breaks the contract.
 * @param[out] device Receives the device, or NULL on failure; required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_AUDIO_DEVICE_OK; #TB_AUDIO_DEVICE_UNAVAILABLE when there is no
 * output device; #TB_AUDIO_DEVICE_NOT_FOUND when none has the identifier;
 * #TB_AUDIO_DEVICE_SYSTEM_ERROR when the system refuses.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_audio_device_result tb_audio_device_open(tb_audio_system *system,
                                                   const tb_audio_device_desc *desc,
                                                   tb_audio_device **device,
                                                   tb_audio_device_error *error);

/** @brief Stop a device and close it. @param device The device; NULL is
 * ignored. */
TB_API void tb_audio_device_close(tb_audio_device *device);

/** @brief Start calling the render function. A started device does nothing.
 * @param device Required non-null. */
TB_API void tb_audio_device_start(tb_audio_device *device);

/**
 * @brief Stop calling the render function: once this returns, it is not
 * running and is not called again until the next start. The device plays
 * silence meanwhile. A stopped device does nothing.
 * @param device Required non-null.
 */
TB_API void tb_audio_device_stop(tb_audio_device *device);

/** @brief The rate and channels the device is fed, which never change while
 * it is open. @param device Required non-null.
 * @param[out] format Required non-null. */
TB_API void tb_audio_device_get_format(const tb_audio_device *device, tb_audio_format *format);

/** @brief How far ahead of the speakers its samples are, now: it may change
 * when the device moves to another. @param device Required non-null.
 * @param[out] latency Required non-null. */
TB_API void tb_audio_device_get_latency(tb_audio_device *device, tb_audio_device_latency *latency);

/**
 * @brief Take the next thing that happened to the device. Events wait for
 * this in the order they happened; when the caller takes none for long,
 * the oldest are dropped.
 * @param device Required non-null.
 * @param[out] event Receives the event; required non-null.
 * @return Whether there was one.
 */
TB_API bool tb_audio_device_next_event(tb_audio_device *device, tb_audio_device_event *event);

/** @brief Name a result: "unavailable". @param result Any value.
 * @return A string with static storage duration; "unknown result" for no
 * result.
 * @par Threading Thread-safe. */
TB_API const char *tb_audio_device_result_name(tb_audio_device_result result);

#endif
