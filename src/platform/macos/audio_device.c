/* macOS audio: Core Audio's HAL output unit, with CoreAudio, AudioToolbox
 * and CoreFoundation loaded at run time from /System/Library/Frameworks.
 *
 * The engine builds against no framework headers, so the part of Core
 * Audio it calls is declared below: its C functions, structures, and the
 * four-character codes of the properties, from the SDK's
 * CoreAudio/AudioHardware.h, AudioToolbox/AudioUnitProperties.h and
 * CoreAudioTypes.h. Each stream is a HAL output unit (kAudioUnitSubType_
 * HALOutput) on one device, fed floats in the caller's format, which the
 * unit converts to the device's; its render callback runs on Core Audio's
 * real-time I/O thread. A stream follows the default output itself:
 * property listeners say when the default changes or the device dies, and
 * a thread of the stream's own moves the unit to the default, so that no
 * work is done on Core Audio's notification thread. */
#include "../shared/audio_device.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

#include <stdint.h>
#include <string.h>

/* A four-character code, as Core Audio's constants are. */
#define TB_CA_CODE(a, b, c, d)                                                                     \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define TB_CA_SYSTEM_OBJECT 1u
#define TB_CA_SCOPE_GLOBAL TB_CA_CODE('g', 'l', 'o', 'b')
#define TB_CA_SCOPE_OUTPUT TB_CA_CODE('o', 'u', 't', 'p')
#define TB_CA_ELEMENT_MAIN 0u
#define TB_CA_DEVICES TB_CA_CODE('d', 'e', 'v', '#')
#define TB_CA_DEFAULT_OUTPUT TB_CA_CODE('d', 'O', 'u', 't')
#define TB_CA_NAME TB_CA_CODE('l', 'n', 'a', 'm')
#define TB_CA_UID TB_CA_CODE('u', 'i', 'd', ' ')
#define TB_CA_NOMINAL_RATE TB_CA_CODE('n', 's', 'r', 't')
#define TB_CA_STREAM_CONFIGURATION TB_CA_CODE('s', 'l', 'a', 'y')
#define TB_CA_BUFFER_FRAME_SIZE TB_CA_CODE('f', 's', 'i', 'z')
#define TB_CA_LATENCY TB_CA_CODE('l', 't', 'n', 'c')
#define TB_CA_SAFETY_OFFSET TB_CA_CODE('s', 'a', 'f', 't')
#define TB_CA_IS_ALIVE TB_CA_CODE('l', 'i', 'v', 'n')

#define TB_AU_TYPE_OUTPUT TB_CA_CODE('a', 'u', 'o', 'u')
#define TB_AU_SUBTYPE_HAL_OUTPUT TB_CA_CODE('a', 'h', 'a', 'l')
#define TB_AU_MANUFACTURER_APPLE TB_CA_CODE('a', 'p', 'p', 'l')
#define TB_AU_STREAM_FORMAT 8u
#define TB_AU_SET_RENDER_CALLBACK 23u
#define TB_AU_CURRENT_DEVICE 2000u
#define TB_AU_SCOPE_GLOBAL 0u
#define TB_AU_SCOPE_INPUT 1u
#define TB_AU_FORMAT_LINEAR_PCM TB_CA_CODE('l', 'p', 'c', 'm')
#define TB_AU_FLAG_FLOAT 1u
#define TB_AU_FLAG_PACKED 8u

/* kCFStringEncodingUTF8 */
#define TB_CF_UTF8 0x08000100u

/* How long a stream with no device waits to look again, when no
 * notification says one came. */
#define TB_CA_RETRY_NANOSECONDS 1000000000u

#define TB_CA_MAX_DEVICES 64u

typedef int32_t tb_ca_status;
typedef uint32_t tb_ca_object;
typedef void *tb_cf_string;
typedef void *tb_au_component;
typedef void *tb_au_unit;

typedef struct tb_ca_address
{
    uint32_t selector;
    uint32_t scope;
    uint32_t element;
} tb_ca_address;

typedef struct tb_ca_stream_format
{
    double sample_rate;
    uint32_t format;
    uint32_t flags;
    uint32_t bytes_per_packet;
    uint32_t frames_per_packet;
    uint32_t bytes_per_frame;
    uint32_t channels;
    uint32_t bits;
    uint32_t reserved;
} tb_ca_stream_format;

typedef struct tb_au_description
{
    uint32_t type;
    uint32_t subtype;
    uint32_t manufacturer;
    uint32_t flags;
    uint32_t flags_mask;
} tb_au_description;

typedef struct tb_ca_buffer
{
    uint32_t channels;
    uint32_t size;
    void *data;
} tb_ca_buffer;

typedef struct tb_ca_buffer_list
{
    uint32_t count;
    tb_ca_buffer buffers[1];
} tb_ca_buffer_list;

typedef tb_ca_status tb_au_render_proc(void *context, uint32_t *flags, const void *time,
                                       uint32_t bus, uint32_t frames, tb_ca_buffer_list *data);

typedef struct tb_au_render_callback
{
    tb_au_render_proc *proc;
    void *context;
} tb_au_render_callback;

typedef tb_ca_status tb_ca_listener(tb_ca_object object, uint32_t count,
                                    const tb_ca_address *addresses, void *context);

typedef struct tb_core_audio
{
    tb_library *core_audio;
    tb_library *toolbox;
    tb_library *foundation;
    tb_ca_status (*get_size)(tb_ca_object object, const tb_ca_address *address,
                             uint32_t qualifier_size, const void *qualifier, uint32_t *size);
    tb_ca_status (*get)(tb_ca_object object, const tb_ca_address *address,
                        uint32_t qualifier_size, const void *qualifier, uint32_t *size,
                        void *data);
    tb_ca_status (*set)(tb_ca_object object, const tb_ca_address *address,
                        uint32_t qualifier_size, const void *qualifier, uint32_t size,
                        const void *data);
    tb_ca_status (*add_listener)(tb_ca_object object, const tb_ca_address *address,
                                 tb_ca_listener *listener, void *context);
    tb_ca_status (*remove_listener)(tb_ca_object object, const tb_ca_address *address,
                                    tb_ca_listener *listener, void *context);
    tb_au_component (*find_next)(tb_au_component component, const tb_au_description *desc);
    tb_ca_status (*instance_new)(tb_au_component component, tb_au_unit *unit);
    tb_ca_status (*instance_dispose)(tb_au_unit unit);
    tb_ca_status (*unit_set)(tb_au_unit unit, uint32_t property, uint32_t scope,
                             uint32_t element, const void *data, uint32_t size);
    tb_ca_status (*unit_initialize)(tb_au_unit unit);
    tb_ca_status (*unit_uninitialize)(tb_au_unit unit);
    tb_ca_status (*output_start)(tb_au_unit unit);
    tb_ca_status (*output_stop)(tb_au_unit unit);
    bool (*string_get)(tb_cf_string string, char *buffer, long size, uint32_t encoding);
    void (*release)(const void *object);
} tb_core_audio;

typedef struct tb_ca_stream
{
    tb_core_audio *audio;
    tb_audio_device *device;
    tb_au_unit unit;
    tb_ca_object playing; /* The device the unit plays on; 0 for none. */
    bool follows_default; /* Once the device asked for goes, it follows the default. */
    tb_audio_format format;
    uint32_t period_frames;
    tb_thread thread;
    tb_semaphore wake;
    tb_platform_atomic_u32 quit;
    tb_platform_atomic_u32 reroute;
} tb_ca_stream;

static tb_ca_address tb_ca_global(uint32_t selector)
{
    return (tb_ca_address){selector, TB_CA_SCOPE_GLOBAL, TB_CA_ELEMENT_MAIN};
}

static tb_ca_address tb_ca_output(uint32_t selector)
{
    return (tb_ca_address){selector, TB_CA_SCOPE_OUTPUT, TB_CA_ELEMENT_MAIN};
}

/* coverage-off: listing and playing need an output device, which CI's runners have none of */
static uint32_t tb_ca_get_u32(const tb_core_audio *audio, tb_ca_object object,
                              tb_ca_address address)
{
    uint32_t value = 0;
    uint32_t size = sizeof(value);

    if (audio->get(object, &address, 0, NULL, &size, &value) != 0)
    {
        return 0;
    }
    return value;
}

static double tb_ca_get_f64(const tb_core_audio *audio, tb_ca_object object,
                            tb_ca_address address)
{
    double value = 0.0;
    uint32_t size = sizeof(value);

    if (audio->get(object, &address, 0, NULL, &size, &value) != 0)
    {
        return 0.0;
    }
    return value;
}

/* A string property of a device, in UTF-8; "" where it has none. */
static void tb_ca_get_text(const tb_core_audio *audio, tb_ca_object object, uint32_t selector,
                           char *text, size_t capacity)
{
    const tb_ca_address address = tb_ca_global(selector);
    tb_cf_string string = NULL;
    uint32_t size = sizeof(string);
    char converted[TB_AUDIO_DEVICE_ID_CAPACITY * 2] = {0};

    text[0] = '\0';
    if (audio->get(object, &address, 0, NULL, &size, &string) != 0 || string == NULL)
    {
        return;
    }
    if (audio->string_get(string, converted, (long)sizeof(converted), TB_CF_UTF8))
    {
        tb_audio_device_copy_name(text, capacity, converted, tb_str_length(converted));
    }
    audio->release(string);
}

/* The channels a device has for output: those of its output streams. */
static uint32_t tb_ca_output_channels(const tb_core_audio *audio, tb_ca_object object)
{
    const tb_ca_address address = tb_ca_output(TB_CA_STREAM_CONFIGURATION);
    uint32_t size = 0;
    uint32_t channels = 0;

    if (audio->get_size(object, &address, 0, NULL, &size) != 0 ||
        size < sizeof(tb_ca_buffer_list))
    {
        return 0;
    }
    tb_ca_buffer_list *const list = tb_calloc(1, size);
    if (audio->get(object, &address, 0, NULL, &size, list) == 0)
    {
        const size_t room =
            (size - offsetof(tb_ca_buffer_list, buffers)) / sizeof(list->buffers[0]);
        for (size_t index = 0; index < list->count && index < room; index++)
        {
            channels += list->buffers[index].channels;
        }
    }
    tb_free(list);
    return channels;
}

/* The system's devices, into `objects`; returns how many. */
static size_t tb_ca_devices(const tb_core_audio *audio, tb_ca_object *objects)
{
    const tb_ca_address address = tb_ca_global(TB_CA_DEVICES);
    uint32_t size = TB_CA_MAX_DEVICES * sizeof(objects[0]);

    if (audio->get(TB_CA_SYSTEM_OBJECT, &address, 0, NULL, &size, objects) != 0)
    {
        return 0;
    }
    return size / sizeof(objects[0]);
}

static tb_ca_object tb_ca_default_output(const tb_core_audio *audio)
{
    return tb_ca_get_u32(audio, TB_CA_SYSTEM_OBJECT, tb_ca_global(TB_CA_DEFAULT_OUTPUT));
}

/* The output device whose UID is `id`; 0 for none. */
static tb_ca_object tb_ca_find(const tb_core_audio *audio, const char *id)
{
    tb_ca_object objects[TB_CA_MAX_DEVICES] = {0};
    const size_t count = tb_ca_devices(audio, objects);

    for (size_t index = 0; index < count; index++)
    {
        char uid[TB_AUDIO_DEVICE_ID_CAPACITY] = {0};
        tb_ca_get_text(audio, objects[index], TB_CA_UID, uid, sizeof(uid));
        if (tb_str_equal(uid, id) && tb_ca_output_channels(audio, objects[index]) > 0)
        {
            return objects[index];
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

static void tb_ca_close(void *native)
{
    tb_core_audio *const audio = native;

    tb_library_close(audio->foundation);
    tb_library_close(audio->toolbox);
    tb_library_close(audio->core_audio);
    tb_free(audio);
}

static void tb_ca_info(void *native, tb_audio_system_info *info)
{
    (void)native;
    memcpy(info->backend, "Core Audio", sizeof("Core Audio"));
    info->follows_default = true;
    info->reports_changes = true;
    info->knows_default = true;
}

static size_t tb_ca_list(void *native, tb_audio_device_info *devices, size_t capacity)
{
    const tb_core_audio *const audio = native;
    tb_ca_object objects[TB_CA_MAX_DEVICES] = {0};
    const size_t count = tb_ca_devices(audio, objects);
    const tb_ca_object default_output = tb_ca_default_output(audio);
    size_t outputs = 0;

    for (size_t index = 0; index < count; index++)
    {
        const uint32_t channels = tb_ca_output_channels(audio, objects[index]);
        if (channels == 0)
        {
            continue;
        }
        if (outputs < capacity)
        {
            tb_audio_device_info *const info = &devices[outputs];
            tb_ca_get_text(audio, objects[index], TB_CA_UID, info->id, sizeof(info->id));
            tb_ca_get_text(audio, objects[index], TB_CA_NAME, info->name, sizeof(info->name));
            info->is_default = objects[index] == default_output;
            info->sample_rate = (uint32_t)(tb_ca_get_f64(audio, objects[index],
                                                         tb_ca_global(TB_CA_NOMINAL_RATE)) +
                                           0.5);
            info->channels = channels;
        }
        outputs++;
    }
    return outputs;
}

/* ---------------------------------------------------------------------------
 * Streams.
 * ------------------------------------------------------------------------- */

/* On Core Audio's I/O thread. */
static tb_ca_status tb_ca_render(void *context, uint32_t *flags, const void *time, uint32_t bus,
                                 uint32_t frames, tb_ca_buffer_list *data)
{
    tb_ca_stream *const stream = context;
    const uint32_t channels = stream->format.channels;

    (void)flags;
    (void)time;
    (void)bus;
    if (data == NULL || data->count < 1 || data->buffers[0].data == NULL)
    {
        return 0;
    }
    const size_t room = data->buffers[0].size / (channels * sizeof(float));
    tb_audio_device_render(stream->device, data->buffers[0].data, frames < room ? frames : room,
                           channels);
    return 0;
}

/* A listener's, on Core Audio's notification thread: wake the stream's
 * thread to look at where it plays. */
static tb_ca_status tb_ca_changed(tb_ca_object object, uint32_t count,
                                  const tb_ca_address *addresses, void *context)
{
    tb_ca_stream *const stream = context;

    (void)object;
    (void)count;
    (void)addresses;
    tb_platform_atomic_u32_store(&stream->reroute, 1u);
    tb_semaphore_signal(&stream->wake, 1);
    return 0;
}

static void tb_ca_watch(tb_ca_stream *stream, tb_ca_object object, bool watch)
{
    const tb_core_audio *const audio = stream->audio;
    const tb_ca_address alive = tb_ca_global(TB_CA_IS_ALIVE);

    if (object == 0)
    {
        return;
    }
    if (watch)
    {
        (void)audio->add_listener(object, &alive, tb_ca_changed, stream);
    }
    else
    {
        (void)audio->remove_listener(object, &alive, tb_ca_changed, stream);
    }
}

static void tb_ca_dispose(tb_ca_stream *stream)
{
    const tb_core_audio *const audio = stream->audio;

    if (stream->unit != NULL)
    {
        (void)audio->output_stop(stream->unit);
        (void)audio->unit_uninitialize(stream->unit);
        (void)audio->instance_dispose(stream->unit);
        stream->unit = NULL;
    }
    tb_ca_watch(stream, stream->playing, false);
    stream->playing = 0;
}

/* Make the unit on `object`, in the stream's format, and start it. */
static tb_audio_device_result tb_ca_start_unit(tb_ca_stream *stream, tb_ca_object object,
                                               tb_audio_device_error *error)
{
    const tb_core_audio *const audio = stream->audio;
    const tb_au_description description = {TB_AU_TYPE_OUTPUT, TB_AU_SUBTYPE_HAL_OUTPUT,
                                           TB_AU_MANUFACTURER_APPLE, 0, 0};
    const tb_au_render_callback callback = {tb_ca_render, stream};
    const double device_rate = tb_ca_get_f64(audio, object, tb_ca_global(TB_CA_NOMINAL_RATE));
    const tb_au_component component = audio->find_next(NULL, &description);
    tb_ca_status status = 0;

    if (stream->format.sample_rate == 0)
    {
        stream->format.sample_rate =
            device_rate >= TB_AUDIO_DEVICE_MIN_RATE ? (uint32_t)(device_rate + 0.5) : 48000u;
    }
    if (stream->format.channels == 0)
    {
        const uint32_t channels = tb_ca_output_channels(audio, object);
        stream->format.channels = channels < TB_AUDIO_DEVICE_MAX_CHANNELS ? channels
                                                                          : TB_AUDIO_DEVICE_MAX_CHANNELS;
    }
    const uint32_t frame_bytes = stream->format.channels * (uint32_t)sizeof(float);
    const tb_ca_stream_format format = {(double)stream->format.sample_rate,
                                        TB_AU_FORMAT_LINEAR_PCM,
                                        TB_AU_FLAG_FLOAT | TB_AU_FLAG_PACKED,
                                        frame_bytes,
                                        1,
                                        frame_bytes,
                                        stream->format.channels,
                                        32,
                                        0};
    if (component == NULL)
    {
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                    "Core Audio: no HAL output unit");
    }
    status = audio->instance_new(component, &stream->unit);
    if (status != 0)
    {
        stream->unit = NULL;
        goto failed;
    }
    status = audio->unit_set(stream->unit, TB_AU_CURRENT_DEVICE, TB_AU_SCOPE_GLOBAL, 0, &object,
                             sizeof(object));
    if (status == 0)
    {
        status = audio->unit_set(stream->unit, TB_AU_STREAM_FORMAT, TB_AU_SCOPE_INPUT, 0, &format,
                                 sizeof(format));
    }
    if (status == 0)
    {
        status = audio->unit_set(stream->unit, TB_AU_SET_RENDER_CALLBACK, TB_AU_SCOPE_INPUT, 0,
                                 &callback, sizeof(callback));
    }
    if (status == 0 && stream->period_frames != 0)
    {
        /* A hint: the device takes the smallest period its clients ask. */
        const tb_ca_address address = tb_ca_global(TB_CA_BUFFER_FRAME_SIZE);
        (void)audio->set(object, &address, 0, NULL, sizeof(stream->period_frames),
                         &stream->period_frames);
    }
    if (status == 0)
    {
        status = audio->unit_initialize(stream->unit);
    }
    if (status == 0)
    {
        status = audio->output_start(stream->unit);
    }
    if (status != 0)
    {
        goto failed;
    }
    stream->playing = object;
    tb_ca_watch(stream, object, true);
    const uint32_t period = tb_ca_get_u32(audio, object, tb_ca_global(TB_CA_BUFFER_FRAME_SIZE));
    const uint32_t delay = tb_ca_get_u32(audio, object, tb_ca_output(TB_CA_LATENCY)) +
                           tb_ca_get_u32(audio, object, tb_ca_output(TB_CA_SAFETY_OFFSET));
    const double rate = device_rate > 0.0 ? device_rate : (double)stream->format.sample_rate;
    /* The period is the device's frames; the buffer is counted at the stream's rate. */
    const uint32_t stream_period =
        (uint32_t)((double)period * (double)stream->format.sample_rate / rate + 0.5);
    const tb_audio_device_latency latency = tb_audio_device_latency_of(
        stream_period, stream_period, stream->format.sample_rate, (double)delay / rate);
    tb_audio_device_set_latency(stream->device, &latency);
    return TB_AUDIO_DEVICE_OK;

failed:
    tb_ca_dispose(stream);
    return tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                "Core Audio: the output unit refused the stream (%d)",
                                (int)status);
}

/* Move to the default output, after it changed or the device went. */
static void tb_ca_reroute(tb_ca_stream *stream, bool *lost)
{
    char name[TB_AUDIO_DEVICE_NAME_CAPACITY] = {0};
    const tb_core_audio *const audio = stream->audio;
    const bool alive =
        stream->playing != 0 && tb_ca_get_u32(audio, stream->playing, tb_ca_global(TB_CA_IS_ALIVE));
    const tb_ca_object target = tb_ca_default_output(audio);

    if (alive && !stream->follows_default)
    {
        return;
    }
    stream->follows_default = true;
    if (alive && target == stream->playing)
    {
        return;
    }
    tb_ca_dispose(stream);
    if (target == 0 || tb_ca_start_unit(stream, target, NULL) != TB_AUDIO_DEVICE_OK)
    {
        if (!*lost)
        {
            tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
        }
        *lost = true;
        return;
    }
    *lost = false;
    tb_ca_get_text(audio, target, TB_CA_NAME, name, sizeof(name));
    tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED, name);
}

static void tb_ca_stream_main(void *argument)
{
    tb_ca_stream *const stream = argument;
    bool lost = false;

    while (tb_platform_atomic_u32_load(&stream->quit) == 0u)
    {
        (void)tb_semaphore_wait_for(&stream->wake, TB_CA_RETRY_NANOSECONDS);
        if (tb_platform_atomic_u32_load(&stream->quit) != 0u)
        {
            break;
        }
        const bool reroute = tb_platform_atomic_u32_exchange(&stream->reroute, 0u) != 0u;
        if (reroute || lost)
        {
            tb_ca_reroute(stream, &lost);
        }
    }
}

static tb_audio_device_result tb_ca_open_stream(void *native, tb_audio_device_request *request,
                                                tb_audio_device *device, void **opened,
                                                tb_audio_device_error *error)
{
    tb_core_audio *const audio = native;
    tb_ca_stream *const stream = tb_calloc(1, sizeof(*stream));
    const tb_ca_address default_output = tb_ca_global(TB_CA_DEFAULT_OUTPUT);
    const tb_thread_options options = {.name = "audio device"};
    tb_ca_object object = 0;

    stream->audio = audio;
    stream->device = device;
    stream->format = request->format;
    stream->period_frames = request->period_frames;
    stream->follows_default = request->device_id[0] == '\0';
    if (stream->follows_default)
    {
        object = tb_ca_default_output(audio);
        if (object == 0)
        {
            tb_free(stream);
            return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                        "Core Audio: no output device");
        }
    }
    else
    {
        object = tb_ca_find(audio, request->device_id);
        if (object == 0)
        {
            tb_free(stream);
            return tb_audio_device_fail(error, TB_AUDIO_DEVICE_NOT_FOUND,
                                        "Core Audio: no output device has the UID '%s'",
                                        request->device_id);
        }
    }
    const tb_audio_device_result result = tb_ca_start_unit(stream, object, error);
    if (result != TB_AUDIO_DEVICE_OK)
    {
        tb_free(stream);
        return result;
    }
    (void)audio->add_listener(TB_CA_SYSTEM_OBJECT, &default_output, tb_ca_changed, stream);
    tb_thread_create_with_options(&stream->thread, &options, tb_ca_stream_main, stream);
    request->format = stream->format;
    *opened = stream;
    return TB_AUDIO_DEVICE_OK;
}

static void tb_ca_close_stream(void *native, void *opened)
{
    tb_core_audio *const audio = native;
    tb_ca_stream *const stream = opened;
    const tb_ca_address default_output = tb_ca_global(TB_CA_DEFAULT_OUTPUT);

    (void)audio->remove_listener(TB_CA_SYSTEM_OBJECT, &default_output, tb_ca_changed, stream);
    tb_ca_watch(stream, stream->playing, false);
    tb_platform_atomic_u32_store(&stream->quit, 1u);
    tb_semaphore_signal(&stream->wake, 1);
    tb_thread_join(&stream->thread);
    tb_ca_dispose(stream);
    tb_free(stream);
}
/* coverage-on */

static const tb_audio_system_ops tb_ca_ops = {tb_ca_close, tb_ca_info, tb_ca_list,
                                              tb_ca_open_stream, tb_ca_close_stream};

static tb_library_result tb_ca_load(const char *name, tb_library **library,
                                    const tb_library_binding *bindings, size_t count,
                                    tb_library_error *error)
{
    const tb_library_result result = tb_library_open(name, TB_LIBRARY_SEARCH_SYSTEM, library, error);

    if (result != TB_LIBRARY_OK)
    {
        return result;
    }
    return tb_library_bind(*library, bindings, count, error);
}

tb_audio_device_result tb_native_audio_open(const tb_audio_system_ops **ops, void **native,
                                            tb_audio_device_error *error)
{
    tb_core_audio *const audio = tb_calloc(1, sizeof(*audio));
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    const tb_library_binding core_audio[] = {
        {"AudioObjectGetPropertyDataSize", (tb_library_function *)&audio->get_size, true},
        {"AudioObjectGetPropertyData", (tb_library_function *)&audio->get, true},
        {"AudioObjectSetPropertyData", (tb_library_function *)&audio->set, true},
        {"AudioObjectAddPropertyListener", (tb_library_function *)&audio->add_listener, true},
        {"AudioObjectRemovePropertyListener", (tb_library_function *)&audio->remove_listener,
         true},
    };
    const tb_library_binding toolbox[] = {
        {"AudioComponentFindNext", (tb_library_function *)&audio->find_next, true},
        {"AudioComponentInstanceNew", (tb_library_function *)&audio->instance_new, true},
        {"AudioComponentInstanceDispose", (tb_library_function *)&audio->instance_dispose, true},
        {"AudioUnitSetProperty", (tb_library_function *)&audio->unit_set, true},
        {"AudioUnitInitialize", (tb_library_function *)&audio->unit_initialize, true},
        {"AudioUnitUninitialize", (tb_library_function *)&audio->unit_uninitialize, true},
        {"AudioOutputUnitStart", (tb_library_function *)&audio->output_start, true},
        {"AudioOutputUnitStop", (tb_library_function *)&audio->output_stop, true},
    };
    const tb_library_binding foundation[] = {
        {"CFStringGetCString", (tb_library_function *)&audio->string_get, true},
        {"CFRelease", (tb_library_function *)&audio->release, true},
    };

    if (tb_ca_load("CoreAudio.framework/CoreAudio", &audio->core_audio, core_audio,
                   sizeof(core_audio) / sizeof(core_audio[0]), &library_error) != TB_LIBRARY_OK ||
        tb_ca_load("AudioToolbox.framework/AudioToolbox", &audio->toolbox, toolbox,
                   sizeof(toolbox) / sizeof(toolbox[0]), &library_error) != TB_LIBRARY_OK ||
        tb_ca_load("CoreFoundation.framework/CoreFoundation", &audio->foundation, foundation,
                   sizeof(foundation) / sizeof(foundation[0]), &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: the frameworks are part of every macOS */
        tb_ca_close(audio);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "Core Audio: %s",
                                    library_error.message);
        /* coverage-on */
    }
    *ops = &tb_ca_ops;
    *native = audio;
    return TB_AUDIO_DEVICE_OK;
}
