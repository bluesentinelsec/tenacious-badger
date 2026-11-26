/* Linux audio through ALSA: libasound.so.2, loaded at run time, for a
 * system where no PipeWire daemon runs.
 *
 * The engine builds with no ALSA headers, so the few functions it calls
 * are declared below, with the values of alsa/pcm.h they take. Each stream
 * has a thread of its own, which renders a period at a time into a buffer
 * of its own and writes it to the PCM, blocking until there is room:
 * ALSA's write is its clock. Opened by name, a stream plays there until
 * the device goes (the write says -ENODEV), then on "default"; with no
 * device at all it tries again every second. */
#include "audio_linux.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* alsa/pcm.h's values. */
#define TB_SND_PCM_STREAM_PLAYBACK 0
#define TB_SND_PCM_FORMAT_FLOAT_LE 14
#define TB_SND_PCM_ACCESS_RW_INTERLEAVED 3

#define TB_ALSA_DEFAULT_DEVICE "default"
#define TB_ALSA_DEFAULT_RATE 48000u
#define TB_ALSA_DEFAULT_CHANNELS 2u
/* The buffer ALSA is asked for, as a latency: in microseconds, and in
 * periods of the hint when one is given. */
#define TB_ALSA_DEFAULT_LATENCY_MICROSECONDS 40000u
#define TB_ALSA_PERIODS_PER_BUFFER 4u
#define TB_ALSA_MICROSECONDS_PER_SECOND 1000000.0

/* How long a stream with no device waits to look again. */
#define TB_ALSA_RETRY_NANOSECONDS 1000000000u

typedef struct tb_snd_pcm tb_snd_pcm;

typedef struct tb_alsa
{
    tb_library *library;
    int (*pcm_open)(tb_snd_pcm **pcm, const char *name, int stream, int mode);
    int (*pcm_set_params)(tb_snd_pcm *pcm, int format, int access, unsigned int channels,
                          unsigned int rate, int soft_resample, unsigned int latency);
    int (*pcm_get_params)(tb_snd_pcm *pcm, unsigned long *buffer_size,
                          unsigned long *period_size);
    long (*pcm_writei)(tb_snd_pcm *pcm, const void *buffer, unsigned long size);
    int (*pcm_recover)(tb_snd_pcm *pcm, int error, int silent);
    int (*pcm_close)(tb_snd_pcm *pcm);
    const char *(*strerror)(int error);
    int (*device_name_hint)(int card, const char *interface, void ***hints);
    char *(*device_name_get_hint)(const void *hint, const char *id);
    int (*device_name_free_hint)(void **hints);
} tb_alsa;

typedef struct tb_alsa_stream
{
    tb_alsa *alsa;
    tb_audio_device *device;
    tb_thread thread;
    tb_platform_atomic_u32 quit;
    tb_snd_pcm *pcm;
    char name[TB_AUDIO_DEVICE_ID_CAPACITY]; /* The device it plays on, or wants to. */
    tb_audio_format format;
    uint32_t period_hint;
    unsigned long period_frames;
    unsigned long buffer_frames;
    float *samples; /* A period's, of the largest period a device gives. */
    size_t sample_capacity;
} tb_alsa_stream;

static void tb_alsa_close(void *native)
{
    tb_alsa *const alsa = native;

    tb_library_close(alsa->library);
    tb_free(alsa);
}

static void tb_alsa_info(void *native, tb_audio_system_info *info)
{
    (void)native;
    memcpy(info->backend, "ALSA", sizeof("ALSA"));
    info->follows_default = true;
    info->reports_changes = true;
    info->knows_default = true;
}

/* A hint's text, which ALSA allocates with the C library's malloc and the
 * caller releases with its free. */
static void tb_alsa_hint(const tb_alsa *alsa, const void *hint, const char *id, char *text,
                         size_t capacity)
{
    char *const value = alsa->device_name_get_hint(hint, id);

    text[0] = '\0';
    if (value == NULL)
    {
        return;
    }
    /* The description's lines: its first is the card, which is enough. */
    size_t length = tb_str_find_char(value, 0, TB_STR_END, '\n');
    if (length == TB_STR_NOT_FOUND)
    {
        length = tb_str_length(value);
    }
    tb_audio_device_copy_name(text, capacity, value, length);
    free(value);
}

/* Whether a hint is an output: its IOID is "Output" or absent, for both. */
static bool tb_alsa_hint_is_output(const tb_alsa *alsa, const void *hint)
{
    char direction[16] = {0};

    tb_alsa_hint(alsa, hint, "IOID", direction, sizeof(direction));
    return direction[0] == '\0' || tb_str_equal(direction, "Output");
}

static size_t tb_alsa_list(void *native, tb_audio_device_info *devices, size_t capacity)
{
    const tb_alsa *const alsa = native;
    void **hints = NULL;
    size_t count = 0;

    if (alsa->device_name_hint(-1, "pcm", &hints) < 0)
    {
        /* coverage-off: ALSA fails to list only when out of memory */
        return 0;
        /* coverage-on */
    }
    for (size_t index = 0; hints[index] != NULL; index++)
    {
        if (!tb_alsa_hint_is_output(alsa, hints[index]))
        {
            continue;
        }
        if (count < capacity)
        {
            tb_audio_device_info *const info = &devices[count];
            tb_alsa_hint(alsa, hints[index], "NAME", info->id, sizeof(info->id));
            tb_alsa_hint(alsa, hints[index], "DESC", info->name, sizeof(info->name));
            info->is_default = tb_str_equal(info->id, TB_ALSA_DEFAULT_DEVICE);
        }
        count++;
    }
    (void)alsa->device_name_free_hint(hints);
    return count;
}

/* Whether the system lists a device of that name. */
static bool tb_alsa_listed(tb_alsa *alsa, const char *name)
{
    tb_audio_device_info devices[64] = {0};
    const size_t count = tb_alsa_list(alsa, devices, sizeof(devices) / sizeof(devices[0]));

    for (size_t index = 0; index < count && index < sizeof(devices) / sizeof(devices[0]); index++)
    {
        if (tb_str_equal(devices[index].id, name))
        {
            return true;
        }
    }
    return false;
}

static tb_audio_device_result tb_alsa_connect(tb_alsa_stream *stream,
                                              tb_audio_device_error *error)
{
    const tb_alsa *const alsa = stream->alsa;
    const uint32_t rate = stream->format.sample_rate;
    const unsigned int latency =
        stream->period_hint != 0
            ? (unsigned int)((double)stream->period_hint * TB_ALSA_PERIODS_PER_BUFFER *
                             TB_ALSA_MICROSECONDS_PER_SECOND / (double)rate)
            : TB_ALSA_DEFAULT_LATENCY_MICROSECONDS;
    int status = alsa->pcm_open(&stream->pcm, stream->name, TB_SND_PCM_STREAM_PLAYBACK, 0);

    if (status < 0)
    {
        stream->pcm = NULL;
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "ALSA: %s: %s",
                                    stream->name, alsa->strerror(status));
    }
    /* coverage-off: playing needs a sound card, which CI's runners have none of */
    status = alsa->pcm_set_params(stream->pcm, TB_SND_PCM_FORMAT_FLOAT_LE,
                                  TB_SND_PCM_ACCESS_RW_INTERLEAVED, stream->format.channels, rate,
                                  1, latency);
    if (status >= 0)
    {
        status = alsa->pcm_get_params(stream->pcm, &stream->buffer_frames, &stream->period_frames);
    }
    if (status < 0 || stream->period_frames == 0)
    {
        (void)alsa->pcm_close(stream->pcm);
        stream->pcm = NULL;
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR, "ALSA: %s: %s",
                                    stream->name, alsa->strerror(status));
    }
    const size_t needed = (size_t)stream->period_frames * stream->format.channels;
    if (needed > stream->sample_capacity)
    {
        tb_free(stream->samples);
        stream->samples = tb_calloc(needed, sizeof(float));
        stream->sample_capacity = needed;
    }
    const tb_audio_device_latency measured = tb_audio_device_latency_of(
        (uint32_t)stream->period_frames, (uint32_t)stream->buffer_frames, rate, 0.0);
    tb_audio_device_set_latency(stream->device, &measured);
    return TB_AUDIO_DEVICE_OK;
}

static void tb_alsa_disconnect(tb_alsa_stream *stream)
{
    if (stream->pcm != NULL)
    {
        (void)stream->alsa->pcm_close(stream->pcm);
        stream->pcm = NULL;
    }
}

/* Find a device again, after the one played on went: "default" from now on. */
static void tb_alsa_reconnect(tb_alsa_stream *stream, bool *lost)
{
    tb_alsa_disconnect(stream);
    memcpy(stream->name, TB_ALSA_DEFAULT_DEVICE, sizeof(TB_ALSA_DEFAULT_DEVICE));
    if (tb_alsa_connect(stream, NULL) != TB_AUDIO_DEVICE_OK)
    {
        if (!*lost)
        {
            tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
        }
        *lost = true;
        tb_platform_sleep(TB_ALSA_RETRY_NANOSECONDS);
        return;
    }
    *lost = false;
    tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED, stream->name);
}

/* Write a period, waiting for room. Returns whether the device is there. */
static bool tb_alsa_write(tb_alsa_stream *stream)
{
    const tb_alsa *const alsa = stream->alsa;
    size_t written = 0;

    tb_audio_device_render(stream->device, stream->samples, stream->period_frames,
                           stream->format.channels);
    while (written < stream->period_frames)
    {
        const long frames =
            alsa->pcm_writei(stream->pcm, stream->samples + written * stream->format.channels,
                             stream->period_frames - written);
        if (frames == -ENODEV)
        {
            return false;
        }
        if (frames < 0)
        {
            /* An underrun, or a resume after suspending: start over. */
            if (alsa->pcm_recover(stream->pcm, (int)frames, 1) < 0)
            {
                return false;
            }
            continue;
        }
        written += (size_t)frames;
    }
    return true;
}

static void tb_alsa_stream_main(void *argument)
{
    tb_alsa_stream *const stream = argument;
    bool lost = false;

    while (tb_platform_atomic_u32_load(&stream->quit) == 0u)
    {
        if (stream->pcm == NULL || !tb_alsa_write(stream))
        {
            tb_alsa_reconnect(stream, &lost);
        }
    }
}
/* coverage-on */

static void tb_alsa_free_stream(tb_alsa_stream *stream)
{
    tb_alsa_disconnect(stream);
    tb_free(stream->samples);
    tb_free(stream);
}

static tb_audio_device_result tb_alsa_open_stream(void *native, tb_audio_device_request *request,
                                                  tb_audio_device *device, void **opened,
                                                  tb_audio_device_error *error)
{
    tb_alsa *const alsa = native;
    tb_alsa_stream *const stream = tb_calloc(1, sizeof(*stream));
    const tb_thread_options options = {.name = "audio device",
                                       .priority = TB_THREAD_PRIORITY_HIGH};
    tb_audio_device_result result = TB_AUDIO_DEVICE_OK;

    stream->alsa = alsa;
    stream->device = device;
    stream->period_hint = request->period_frames;
    if (request->device_id[0] == '\0')
    {
        memcpy(stream->name, TB_ALSA_DEFAULT_DEVICE, sizeof(TB_ALSA_DEFAULT_DEVICE));
    }
    else if (tb_alsa_listed(alsa, request->device_id))
    {
        /* coverage-off: CI's runners list no device to open by name */
        tb_audio_device_copy_name(stream->name, sizeof(stream->name), request->device_id,
                                  tb_str_length(request->device_id));
        /* coverage-on */
    }
    else
    {
        tb_free(stream);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_NOT_FOUND,
                                    "ALSA: no device is named '%s'", request->device_id);
    }
    stream->format.sample_rate =
        request->format.sample_rate != 0 ? request->format.sample_rate : TB_ALSA_DEFAULT_RATE;
    stream->format.channels =
        request->format.channels != 0 ? request->format.channels : TB_ALSA_DEFAULT_CHANNELS;
    result = tb_alsa_connect(stream, error);
    if (result != TB_AUDIO_DEVICE_OK)
    {
        tb_alsa_free_stream(stream);
        return result;
    }
    /* coverage-off: as above, playing needs a sound card */
    request->format = stream->format;
    tb_thread_create_with_options(&stream->thread, &options, tb_alsa_stream_main, stream);
    *opened = stream;
    return TB_AUDIO_DEVICE_OK;
}

static void tb_alsa_close_stream(void *native, void *opened)
{
    tb_alsa_stream *const stream = opened;

    (void)native;
    tb_platform_atomic_u32_store(&stream->quit, 1u);
    tb_thread_join(&stream->thread);
    tb_alsa_free_stream(stream);
}
/* coverage-on */

static const tb_audio_system_ops tb_alsa_ops = {tb_alsa_close, tb_alsa_info, tb_alsa_list,
                                                tb_alsa_open_stream, tb_alsa_close_stream};

tb_audio_device_result tb_alsa_open(const tb_audio_system_ops **ops, void **native,
                                    tb_audio_device_error *error)
{
    tb_alsa *const alsa = tb_calloc(1, sizeof(*alsa));
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    const tb_library_binding bindings[] = {
        {"snd_pcm_open", (tb_library_function *)&alsa->pcm_open, true},
        {"snd_pcm_set_params", (tb_library_function *)&alsa->pcm_set_params, true},
        {"snd_pcm_get_params", (tb_library_function *)&alsa->pcm_get_params, true},
        {"snd_pcm_writei", (tb_library_function *)&alsa->pcm_writei, true},
        {"snd_pcm_recover", (tb_library_function *)&alsa->pcm_recover, true},
        {"snd_pcm_close", (tb_library_function *)&alsa->pcm_close, true},
        {"snd_strerror", (tb_library_function *)&alsa->strerror, true},
        {"snd_device_name_hint", (tb_library_function *)&alsa->device_name_hint, true},
        {"snd_device_name_get_hint", (tb_library_function *)&alsa->device_name_get_hint, true},
        {"snd_device_name_free_hint", (tb_library_function *)&alsa->device_name_free_hint, true},
    };

    if (tb_library_open("libasound.so.2", TB_LIBRARY_SEARCH_SYSTEM, &alsa->library,
                        &library_error) != TB_LIBRARY_OK ||
        tb_library_bind(alsa->library, bindings, sizeof(bindings) / sizeof(bindings[0]),
                        &library_error) != TB_LIBRARY_OK)
    {
        tb_alsa_close(alsa);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "ALSA: %s",
                                    library_error.message);
    }
    *ops = &tb_alsa_ops;
    *native = alsa;
    return TB_AUDIO_DEVICE_OK;
}
