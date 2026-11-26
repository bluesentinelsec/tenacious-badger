/* What the shared front of the audio device interface and each platform's
 * backend share: the table of functions a backend fills in, what it is
 * asked to open, and the calls by which it feeds the caller's render
 * function and reports what happened. */
#ifndef TB_PLATFORM_SHARED_AUDIO_DEVICE_H
#define TB_PLATFORM_SHARED_AUDIO_DEVICE_H

#include "badger/audio_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a backend is asked to open. */
typedef struct tb_audio_device_request
{
    const char *device_id; /* "" for the default output, which the stream then follows. */
    /* As the caller asked; a zero asks for the device's own. The backend
     * writes what it opened, which is what the render function is fed. */
    tb_audio_format format;
    uint32_t period_frames; /* A hint; 0 for the backend's default. */
    const char *stream_name;
} tb_audio_device_request;

/* What a backend does; each function is required and called on the
 * caller's thread, never on a device's. */
typedef struct tb_audio_system_ops
{
    /* Release the backend's state; its devices are closed. */
    void (*close)(void *native);
    /* Say what the backend is: the front has zeroed `info`. */
    void (*info)(void *native, tb_audio_system_info *info);
    /* Fill up to `capacity` of `devices`, zeroed, and return how many
     * there are. */
    size_t (*list)(void *native, tb_audio_device_info *devices, size_t capacity);
    /* Open a stream for `device`, running from the moment it returns, and
     * give `*stream`. Its thread calls tb_audio_device_render() with every
     * buffer the system wants, and the backend calls
     * tb_audio_device_set_latency() before it returns. On failure nothing
     * is left open. */
    tb_audio_device_result (*open)(void *native, tb_audio_device_request *request,
                                   tb_audio_device *device, void **stream,
                                   tb_audio_device_error *error);
    /* Stop a stream and release it: once this returns, its thread calls
     * tb_audio_device_render() no more. */
    void (*close_stream)(void *native, void *stream);
} tb_audio_system_ops;

/* Connect an audio system to the platform's audio: give `*ops` and
 * `*native`. Each platform adapter defines it. */
tb_audio_device_result tb_native_audio_open(const tb_audio_system_ops **ops, void **native,
                                            tb_audio_device_error *error);

/* Make an audio system over a backend, as tb_audio_system_open() does over
 * the platform's: for tests, with a backend of their own. Never NULL;
 * exhaustion calls the memory failure handler. */
tb_audio_system *tb_audio_system_make(const tb_audio_system_ops *ops, void *native);

/* Fill `frame_count` frames of `channels` samples: the caller's render
 * function's, when the device is started, and silence otherwise. A
 * backend calls it from its stream's thread, and from nowhere else; it
 * never locks or allocates. */
void tb_audio_device_render(tb_audio_device *device, float *samples, size_t frame_count,
                            uint32_t channels);

/* Report what happened to a device, for tb_audio_device_next_event(). It
 * locks, so a backend calls it from any thread but never from a render:
 * from its stream's thread between two buffers, or from a notification.
 * `device_name` may be NULL for none. */
void tb_audio_device_report(tb_audio_device *device, tb_audio_device_event_type type,
                            const char *device_name);

/* Say how far ahead of the speakers the stream is: on opening, and
 * whenever that changes; as tb_audio_device_report(), never from a render. */
void tb_audio_device_set_latency(tb_audio_device *device, const tb_audio_device_latency *latency);

/* Fill in an error, formatted as tb_fmt_buffer() does; returns `result`.
 * `error` may be NULL. */
tb_audio_device_result tb_audio_device_fail(tb_audio_device_error *error,
                                            tb_audio_device_result result, const char *format,
                                            ...);

/* Copy a name the system gave into `name`, of `capacity` bytes: cut at a
 * character to fit, and with any byte that is not valid UTF-8 replaced by
 * '?'. `length` is the source's bytes, or up to its NUL. */
void tb_audio_device_copy_name(char *name, size_t capacity, const char *source, size_t length);

/* The speakers of `channels` channels, as WAVE_FORMAT_EXTENSIBLE's bits:
 * those of tb_sound_default_channel_mask(), the layout every backend opens
 * its streams in. 0 past #TB_AUDIO_DEVICE_MAX_CHANNELS. */
uint32_t tb_audio_device_channel_mask(uint32_t channels);

/* The latency of a stream from its frames and the system's own delay. */
tb_audio_device_latency tb_audio_device_latency_of(uint32_t period_frames, uint32_t buffer_frames,
                                                   uint32_t sample_rate, double device_seconds);

#endif
