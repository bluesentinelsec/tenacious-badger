/* The front of the audio device interface, the same on every platform:
 * the contracts, the gate that keeps the caller's render function from
 * running while a device is stopped, the latency, and the events. Each
 * platform's backend opens the streams through its table. */
#include "audio_device.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/size.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "badger/utf8.h"

#include <stdarg.h>
#include <string.h>

/* Events a device keeps for the caller to take; older ones are dropped. */
#define TB_AUDIO_DEVICE_EVENT_CAPACITY 16u

#define TB_AUDIO_DEVICE_DEFAULT_STREAM_NAME "badger"

struct tb_audio_system
{
    const tb_audio_system_ops *ops;
    void *native;
    size_t open_devices;
};

struct tb_audio_device
{
    tb_audio_system *system;
    void *stream;
    tb_audio_render_function *render;
    void *context;
    tb_audio_format format;
    /* Whether the render function may be called: 1 between start and stop. */
    tb_platform_atomic_u32 started;
    /* 1 while the device's thread is between its check of `started` and the
     * render function's return. Raised before the check, so that stop, which
     * lowers `started` and then waits for this to fall, never misses a call
     * that has begun. */
    tb_platform_atomic_u32 rendering;
    /* The events and the latency, which the backend's threads write. */
    tb_mutex mutex;
    tb_audio_device_event events[TB_AUDIO_DEVICE_EVENT_CAPACITY];
    size_t first_event;
    size_t event_count;
    tb_audio_device_latency latency;
};

tb_audio_device_result tb_audio_device_fail(tb_audio_device_error *error,
                                            tb_audio_device_result result, const char *format, ...)
{
    va_list arguments;

    if (error == NULL)
    {
        return result;
    }
    error->result = result;
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    return result;
}

void tb_audio_device_copy_name(char *name, size_t capacity, const char *source, size_t length)
{
    size_t written = 0;
    size_t offset = 0;

    TB_ASSERT(name != NULL);
    TB_ASSERT(capacity > 0);
    TB_ASSERT(source != NULL || length == 0);
    while (offset < length && source[offset] != '\0')
    {
        uint32_t scalar = 0;
        size_t next = offset + 1;
        size_t size = 1;
        const bool valid = tb_utf8_decode(source, length, offset, &scalar, &next);
        if (valid)
        {
            size = next - offset;
        }
        if (written + size >= capacity)
        {
            break;
        }
        if (valid)
        {
            memcpy(name + written, source + offset, size);
        }
        else
        {
            name[written] = '?';
        }
        written += size;
        offset = valid ? next : offset + 1;
    }
    name[written] = '\0';
}

uint32_t tb_audio_device_channel_mask(uint32_t channels)
{
    /* Front left 0x1, front right 0x2, front centre 0x4, low frequency 0x8,
     * back left 0x10, back right 0x20, back centre 0x100, side left 0x200,
     * side right 0x400. */
    static const uint32_t masks[TB_AUDIO_DEVICE_MAX_CHANNELS + 1] = {
        0x0u, 0x4u, 0x3u, 0x7u, 0x33u, 0x37u, 0x3fu, 0x70fu, 0x63fu};

    if (channels > TB_AUDIO_DEVICE_MAX_CHANNELS)
    {
        return 0;
    }
    return masks[channels];
}

tb_audio_device_latency tb_audio_device_latency_of(uint32_t period_frames, uint32_t buffer_frames,
                                                   uint32_t sample_rate, double device_seconds)
{
    tb_audio_device_latency latency = {0};

    latency.period_frames = period_frames;
    latency.buffer_frames = buffer_frames;
    latency.seconds = device_seconds;
    if (sample_rate > 0)
    {
        latency.seconds += (double)buffer_frames / (double)sample_rate;
    }
    return latency;
}

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

tb_audio_system *tb_audio_system_make(const tb_audio_system_ops *ops, void *native)
{
    tb_audio_system *const system = tb_calloc(1, sizeof(*system));

    TB_ASSERT(ops != NULL);
    system->ops = ops;
    system->native = native;
    return system;
}

tb_audio_device_result tb_audio_system_open(tb_audio_system **system, tb_audio_device_error *error)
{
    const tb_audio_system_ops *ops = NULL;
    void *native = NULL;
    tb_audio_device_result result = TB_AUDIO_DEVICE_OK;

    TB_ASSERT(system != NULL);
    *system = NULL;
    if (error != NULL)
    {
        *error = (tb_audio_device_error)TB_AUDIO_DEVICE_ERROR_INIT;
    }
    result = tb_native_audio_open(&ops, &native, error);
    if (result != TB_AUDIO_DEVICE_OK)
    {
        return result;
    }
    *system = tb_audio_system_make(ops, native);
    return TB_AUDIO_DEVICE_OK;
}

void tb_audio_system_close(tb_audio_system *system)
{
    if (system == NULL)
    {
        return;
    }
    TB_ASSERT(system->open_devices == 0);
    system->ops->close(system->native);
    tb_free(system);
}

void tb_audio_system_get_info(const tb_audio_system *system, tb_audio_system_info *info)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(info != NULL);
    *info = (tb_audio_system_info){0};
    system->ops->info(system->native, info);
}

size_t tb_audio_system_list_devices(tb_audio_system *system, tb_audio_device_info *devices,
                                    size_t capacity)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(devices != NULL || capacity == 0);
    for (size_t index = 0; index < capacity; index++)
    {
        devices[index] = (tb_audio_device_info){0};
    }
    return system->ops->list(system->native, devices, capacity);
}

/* ---------------------------------------------------------------------------
 * Devices.
 * ------------------------------------------------------------------------- */

static bool tb_audio_format_valid(const tb_audio_format *format)
{
    const bool rate_valid =
        format->sample_rate == 0 || (format->sample_rate >= TB_AUDIO_DEVICE_MIN_RATE &&
                                     format->sample_rate <= TB_AUDIO_DEVICE_MAX_RATE);
    const bool channels_valid = format->channels <= TB_AUDIO_DEVICE_MAX_CHANNELS;

    return rate_valid && channels_valid;
}

static bool tb_audio_format_complete(const tb_audio_format *format)
{
    return format->sample_rate != 0 && format->channels != 0 && tb_audio_format_valid(format);
}

tb_audio_device_result tb_audio_device_open(tb_audio_system *system,
                                            const tb_audio_device_desc *desc,
                                            tb_audio_device **device, tb_audio_device_error *error)
{
    tb_audio_device *opened = NULL;
    tb_audio_device_request request = {0};
    tb_audio_device_result result = TB_AUDIO_DEVICE_OK;

    TB_ASSERT(system != NULL);
    TB_ASSERT(desc != NULL);
    TB_ASSERT(device != NULL);
    TB_ASSERT(desc->render != NULL);
    TB_ASSERT(tb_audio_format_valid(&desc->format));
    *device = NULL;
    if (error != NULL)
    {
        *error = (tb_audio_device_error)TB_AUDIO_DEVICE_ERROR_INIT;
    }
    opened = tb_calloc(1, sizeof(*opened));
    opened->system = system;
    opened->render = desc->render;
    opened->context = desc->context;
    request.device_id = desc->device_id != NULL ? desc->device_id : "";
    request.format = desc->format;
    request.period_frames = desc->period_frames;
    request.stream_name =
        desc->stream_name != NULL ? desc->stream_name : TB_AUDIO_DEVICE_DEFAULT_STREAM_NAME;
    result = system->ops->open(system->native, &request, opened, &opened->stream, error);
    if (result != TB_AUDIO_DEVICE_OK)
    {
        tb_free(opened);
        return result;
    }
    TB_ASSERT(tb_audio_format_complete(&request.format));
    opened->format = request.format;
    system->open_devices++;
    *device = opened;
    return TB_AUDIO_DEVICE_OK;
}

void tb_audio_device_close(tb_audio_device *device)
{
    if (device == NULL)
    {
        return;
    }
    tb_audio_device_stop(device);
    device->system->ops->close_stream(device->system->native, device->stream);
    TB_ASSERT(device->system->open_devices > 0);
    device->system->open_devices--;
    tb_free(device);
}

void tb_audio_device_start(tb_audio_device *device)
{
    TB_ASSERT(device != NULL);
    tb_platform_atomic_u32_store(&device->started, 1u);
}

void tb_audio_device_stop(tb_audio_device *device)
{
    TB_ASSERT(device != NULL);
    tb_platform_atomic_u32_store(&device->started, 0u);
    while (tb_platform_atomic_u32_load(&device->rendering) != 0u)
    {
        tb_thread_yield();
    }
}

void tb_audio_device_get_format(const tb_audio_device *device, tb_audio_format *format)
{
    TB_ASSERT(device != NULL);
    TB_ASSERT(format != NULL);
    *format = device->format;
}

void tb_audio_device_get_latency(tb_audio_device *device, tb_audio_device_latency *latency)
{
    TB_ASSERT(device != NULL);
    TB_ASSERT(latency != NULL);
    tb_mutex_lock(&device->mutex);
    *latency = device->latency;
    tb_mutex_unlock(&device->mutex);
}

bool tb_audio_device_next_event(tb_audio_device *device, tb_audio_device_event *event)
{
    bool taken = false;

    TB_ASSERT(device != NULL);
    TB_ASSERT(event != NULL);
    tb_mutex_lock(&device->mutex);
    if (device->event_count > 0)
    {
        *event = device->events[device->first_event];
        device->first_event = (device->first_event + 1) % TB_AUDIO_DEVICE_EVENT_CAPACITY;
        device->event_count--;
        taken = true;
    }
    tb_mutex_unlock(&device->mutex);
    return taken;
}

/* ---------------------------------------------------------------------------
 * What backends call.
 * ------------------------------------------------------------------------- */

void tb_audio_device_render(tb_audio_device *device, float *samples, size_t frame_count,
                            uint32_t channels)
{
    size_t bytes = 0;

    TB_ASSERT(device != NULL);
    if (frame_count == 0)
    {
        return;
    }
    TB_ASSERT(samples != NULL);
    TB_ASSERT(channels > 0 && channels <= TB_AUDIO_DEVICE_MAX_CHANNELS);
    if (!tb_size_multiply(frame_count, channels * sizeof(*samples), &bytes))
    {
        return;
    }
    memset(samples, 0, bytes);
    tb_platform_atomic_u32_store(&device->rendering, 1u);
    if (tb_platform_atomic_u32_load(&device->started) != 0u)
    {
        device->render(device->context, samples, frame_count, channels);
    }
    tb_platform_atomic_u32_store(&device->rendering, 0u);
}

void tb_audio_device_report(tb_audio_device *device, tb_audio_device_event_type type,
                            const char *device_name)
{
    tb_audio_device_event event = {0};

    TB_ASSERT(device != NULL);
    event.type = type;
    if (device_name != NULL)
    {
        tb_audio_device_copy_name(event.device_name, sizeof(event.device_name), device_name,
                                  tb_str_length(device_name));
    }
    tb_mutex_lock(&device->mutex);
    if (device->event_count == TB_AUDIO_DEVICE_EVENT_CAPACITY)
    {
        device->first_event = (device->first_event + 1) % TB_AUDIO_DEVICE_EVENT_CAPACITY;
        device->event_count--;
    }
    device->events[(device->first_event + device->event_count) % TB_AUDIO_DEVICE_EVENT_CAPACITY] =
        event;
    device->event_count++;
    tb_mutex_unlock(&device->mutex);
}

void tb_audio_device_set_latency(tb_audio_device *device, const tb_audio_device_latency *latency)
{
    TB_ASSERT(device != NULL);
    TB_ASSERT(latency != NULL);
    tb_mutex_lock(&device->mutex);
    device->latency = *latency;
    tb_mutex_unlock(&device->mutex);
}
