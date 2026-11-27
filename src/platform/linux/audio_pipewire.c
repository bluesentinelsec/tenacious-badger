/* Linux audio through PipeWire: libpipewire-0.3.so.0, loaded at run time,
 * which the Steam Deck and current desktops run.
 *
 * The engine builds with no PipeWire headers, so the part of its interface
 * the backend uses is declared below: structures of PipeWire's stable ABI
 * (pw_buffer, spa_buffer, the event tables) and the method tables reached
 * through an spa_interface, as PipeWire's inline functions reach them. A
 * thread loop of PipeWire's runs the connection; every call from the
 * caller's thread holds its lock. A registry listener keeps the list of
 * sinks, and the "default" metadata says which is the default, so that
 * listing asks nothing of the daemon.
 *
 * Streams are pw_streams that ask for floats in the caller's format, which
 * PipeWire converts to the sink's, and that process on PipeWire's
 * real-time data thread (PW_STREAM_FLAG_RT_PROCESS). A stream with no
 * target follows the default sink, which PipeWire's session manager
 * (WirePlumber) moves it to; one with a target is moved to the default
 * when its sink goes. The backend sees both in the registry and the
 * metadata, and reports them. */
#include "audio_pipewire.h"
#include "audio_linux.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"

#include <string.h>

/* The most sinks the backend keeps; past it, more are not listed. */
#define TB_PIPEWIRE_MAX_SINKS 64u

/* How long the backend waits for the daemon, in seconds. */
#define TB_PIPEWIRE_TIMEOUT_SECONDS 3

/* How long an opening waits for the first buffer, to measure the latency:
 * in milliseconds, of 1 ms each. */
#define TB_PIPEWIRE_FIRST_BUFFER_MILLISECONDS 500u

#define TB_PIPEWIRE_DEFAULT_RATE 48000u
#define TB_PIPEWIRE_DEFAULT_CHANNELS 2u
/* About 10 ms at 48 kHz, as a quantum. */
#define TB_PIPEWIRE_DEFAULT_PERIOD_SECONDS 0.01

#define TB_PW_ID_ANY 0xffffffffu
#define TB_PW_ID_CORE 0u
#define TB_PW_DIRECTION_OUTPUT 1
#define TB_PW_STREAM_FLAG_AUTOCONNECT (1u << 0)
#define TB_PW_STREAM_FLAG_MAP_BUFFERS (1u << 2)
#define TB_PW_STREAM_FLAG_RT_PROCESS (1u << 4)
#define TB_PW_STREAM_STATE_ERROR (-1)
#define TB_PW_STREAM_STATE_PAUSED 2
#define TB_PW_VERSION_REGISTRY 3u
#define TB_PW_VERSION_METADATA 3u
#define TB_PW_TYPE_NODE "PipeWire:Interface:Node"
#define TB_PW_TYPE_METADATA "PipeWire:Interface:Metadata"

/* ---------------------------------------------------------------------------
 * PipeWire's ABI, as its headers declare it.
 * ------------------------------------------------------------------------- */

struct tb_pw_thread_loop;
struct tb_pw_loop;
struct tb_pw_context;
struct tb_pw_properties;
struct tb_pw_stream;
struct tb_pw_proxy;

struct tb_spa_list
{
    struct tb_spa_list *next;
    struct tb_spa_list *prev;
};

struct tb_spa_callbacks
{
    const void *funcs;
    void *data;
};

struct tb_spa_hook
{
    struct tb_spa_list link;
    struct tb_spa_callbacks cb;
    void (*removed)(struct tb_spa_hook *hook);
    void *priv;
};

/* What a pw_core, pw_registry and pw_metadata begin with. */
struct tb_spa_interface
{
    const char *type;
    uint32_t version;
    struct tb_spa_callbacks cb;
};

struct tb_spa_dict_item
{
    const char *key;
    const char *value;
};

struct tb_spa_dict
{
    uint32_t flags;
    uint32_t n_items;
    const struct tb_spa_dict_item *items;
};

struct tb_spa_chunk
{
    uint32_t offset;
    uint32_t size;
    int32_t stride;
    int32_t flags;
};

struct tb_spa_data
{
    uint32_t type;
    uint32_t flags;
    int64_t fd;
    uint32_t mapoffset;
    uint32_t maxsize;
    void *data;
    struct tb_spa_chunk *chunk;
};

struct tb_spa_buffer
{
    uint32_t n_metas;
    uint32_t n_datas;
    void *metas;
    struct tb_spa_data *datas;
};

struct tb_pw_buffer
{
    struct tb_spa_buffer *buffer;
    void *user_data;
    uint64_t size;
    uint64_t requested; /* Since 0.3.49. */
};

struct tb_pw_time
{
    int64_t now;
    uint32_t rate_num;
    uint32_t rate_denom;
    uint64_t ticks;
    int64_t delay;
    uint64_t queued;
    uint64_t buffered;
    uint32_t queued_buffers;
    uint32_t avail_buffers;
    uint64_t size;
};

struct tb_pw_core_info
{
    uint32_t id;
    uint32_t cookie;
    const char *user_name;
    const char *host_name;
    const char *version;
    const char *name;
    uint64_t change_mask;
    const struct tb_spa_dict *props;
};

struct tb_pw_stream_events
{
    uint32_t version;
    void (*destroy)(void *data);
    void (*state_changed)(void *data, int old, int state, const char *error);
    void (*control_info)(void *data, uint32_t id, const void *control);
    void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
    void (*param_changed)(void *data, uint32_t id, const void *param);
    void (*add_buffer)(void *data, struct tb_pw_buffer *buffer);
    void (*remove_buffer)(void *data, struct tb_pw_buffer *buffer);
    void (*process)(void *data);
    void (*drained)(void *data);
    void (*command)(void *data, const void *command);
    void (*trigger_done)(void *data);
};

struct tb_pw_core_events
{
    uint32_t version;
    void (*info)(void *data, const struct tb_pw_core_info *info);
    void (*done)(void *data, uint32_t id, int seq);
    void (*ping)(void *data, uint32_t id, int seq);
    void (*error)(void *data, uint32_t id, int seq, int res, const char *message);
    void (*remove_id)(void *data, uint32_t id);
    void (*bound_id)(void *data, uint32_t id, uint32_t global_id);
    void (*add_mem)(void *data, uint32_t id, uint32_t type, int fd, uint32_t flags);
    void (*remove_mem)(void *data, uint32_t id);
    void (*bound_props)(void *data, uint32_t id, uint32_t global_id,
                        const struct tb_spa_dict *props);
};

struct tb_pw_core_methods
{
    uint32_t version;
    int (*add_listener)(void *object, struct tb_spa_hook *listener,
                        const struct tb_pw_core_events *events, void *data);
    int (*hello)(void *object, uint32_t version);
    int (*sync)(void *object, uint32_t id, int seq);
    int (*pong)(void *object, uint32_t id, int seq);
    int (*error)(void *object, uint32_t id, int seq, int res, const char *message);
    void *(*get_registry)(void *object, uint32_t version, size_t user_data_size);
};

struct tb_pw_registry_events
{
    uint32_t version;
    void (*global)(void *data, uint32_t id, uint32_t permissions, const char *type,
                   uint32_t version, const struct tb_spa_dict *props);
    void (*global_remove)(void *data, uint32_t id);
};

struct tb_pw_registry_methods
{
    uint32_t version;
    int (*add_listener)(void *object, struct tb_spa_hook *listener,
                        const struct tb_pw_registry_events *events, void *data);
    void *(*bind)(void *object, uint32_t id, const char *type, uint32_t version,
                  size_t user_data_size);
};

struct tb_pw_metadata_events
{
    uint32_t version;
    int (*property)(void *data, uint32_t subject, const char *key, const char *type,
                    const char *value);
};

struct tb_pw_metadata_methods
{
    uint32_t version;
    int (*add_listener)(void *object, struct tb_spa_hook *listener,
                        const struct tb_pw_metadata_events *events, void *data);
};

typedef struct tb_pw_functions
{
    void (*init)(int *argc, char ***argv);
    void (*deinit)(void);
    struct tb_pw_thread_loop *(*thread_loop_new)(const char *name, const struct tb_spa_dict *props);
    void (*thread_loop_destroy)(struct tb_pw_thread_loop *loop);
    int (*thread_loop_start)(struct tb_pw_thread_loop *loop);
    void (*thread_loop_stop)(struct tb_pw_thread_loop *loop);
    void (*thread_loop_lock)(struct tb_pw_thread_loop *loop);
    void (*thread_loop_unlock)(struct tb_pw_thread_loop *loop);
    int (*thread_loop_timed_wait)(struct tb_pw_thread_loop *loop, int seconds);
    void (*thread_loop_signal)(struct tb_pw_thread_loop *loop, bool wait_for_accept);
    struct tb_pw_loop *(*thread_loop_get_loop)(struct tb_pw_thread_loop *loop);
    struct tb_pw_context *(*context_new)(struct tb_pw_loop *loop, struct tb_pw_properties *props,
                                         size_t user_data_size);
    void (*context_destroy)(struct tb_pw_context *context);
    void *(*context_connect)(struct tb_pw_context *context, struct tb_pw_properties *props,
                             size_t user_data_size);
    int (*core_disconnect)(void *core);
    void (*proxy_destroy)(struct tb_pw_proxy *proxy);
    struct tb_pw_properties *(*properties_new)(const char *key, ...);
    struct tb_pw_stream *(*stream_new)(void *core, const char *name,
                                       struct tb_pw_properties *props);
    void (*stream_add_listener)(struct tb_pw_stream *stream, struct tb_spa_hook *listener,
                                const struct tb_pw_stream_events *events, void *data);
    int (*stream_connect)(struct tb_pw_stream *stream, int direction, uint32_t target_id,
                          uint32_t flags, const void **params, uint32_t param_count);
    void (*stream_destroy)(struct tb_pw_stream *stream);
    struct tb_pw_buffer *(*stream_dequeue_buffer)(struct tb_pw_stream *stream);
    int (*stream_queue_buffer)(struct tb_pw_stream *stream, struct tb_pw_buffer *buffer);
    int (*stream_get_time_n)(struct tb_pw_stream *stream, struct tb_pw_time *time, size_t size);
} tb_pw_functions;

/* ---------------------------------------------------------------------------
 * The backend.
 * ------------------------------------------------------------------------- */

typedef struct tb_pipewire_sink
{
    uint32_t id;
    char name[TB_AUDIO_DEVICE_ID_CAPACITY];          /* node.name */
    char description[TB_AUDIO_DEVICE_NAME_CAPACITY]; /* node.description */
    uint32_t rate;
    uint32_t channels;
} tb_pipewire_sink;

typedef struct tb_pipewire_stream tb_pipewire_stream;

typedef struct tb_pipewire
{
    tb_library *library;
    tb_pw_functions pw;
    struct tb_pw_thread_loop *loop;
    struct tb_pw_context *context;
    void *core;
    void *registry;
    void *metadata;
    struct tb_spa_hook core_hook;
    struct tb_spa_hook registry_hook;
    struct tb_spa_hook metadata_hook;
    int sync_seq;
    bool synced;
    bool failed;
    uint32_t clock_rate;
    /* What the registry and the metadata say; the loop's lock guards them. */
    tb_pipewire_sink sinks[TB_PIPEWIRE_MAX_SINKS];
    size_t sink_count;
    char default_sink[TB_AUDIO_DEVICE_ID_CAPACITY];
    tb_pipewire_stream *streams;
} tb_pipewire;

struct tb_pipewire_stream
{
    tb_pipewire *pipewire;
    tb_audio_device *device;
    struct tb_pw_stream *stream;
    struct tb_spa_hook hook;
    tb_pipewire_stream *next;
    uint32_t channels;
    /* The sink it was opened on, by name; "" once it follows the default. */
    char target[TB_AUDIO_DEVICE_ID_CAPACITY];
    bool lost;
    int state;
    /* Set by the data thread: frames it was last asked for, and whether it
     * has been asked at all. */
    tb_platform_atomic_u32 requested;
    tb_platform_atomic_u32 processed;
};

/* coverage-off: needs a PipeWire daemon, which CI's runners have none of */
static void tb_pipewire_lock(tb_pipewire *pipewire)
{
    pipewire->pw.thread_loop_lock(pipewire->loop);
}

static void tb_pipewire_unlock(tb_pipewire *pipewire)
{
    pipewire->pw.thread_loop_unlock(pipewire->loop);
}

static const char *tb_spa_lookup(const struct tb_spa_dict *dict, const char *key)
{
    if (dict == NULL)
    {
        return NULL;
    }
    for (uint32_t index = 0; index < dict->n_items; index++)
    {
        if (tb_str_equal(dict->items[index].key, key))
        {
            return dict->items[index].value;
        }
    }
    return NULL;
}

static uint32_t tb_spa_lookup_number(const struct tb_spa_dict *dict, const char *key)
{
    const char *const text = tb_spa_lookup(dict, key);
    uint32_t value = 0;

    if (text == NULL)
    {
        return 0;
    }
    for (size_t index = 0; text[index] >= '0' && text[index] <= '9' && value < 100000000u; index++)
    {
        value = value * 10u + (uint32_t)(text[index] - '0');
    }
    return value;
}

static void tb_pipewire_copy(char *destination, size_t capacity, const char *source)
{
    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }
    tb_audio_device_copy_name(destination, capacity, source, tb_str_length(source));
}

static const tb_pipewire_sink *tb_pipewire_find_sink(const tb_pipewire *pipewire, const char *name)
{
    for (size_t index = 0; index < pipewire->sink_count; index++)
    {
        if (tb_str_equal(pipewire->sinks[index].name, name))
        {
            return &pipewire->sinks[index];
        }
    }
    return NULL;
}

/* The name for a player of the sink a stream plays on now. */
static const char *tb_pipewire_playing(const tb_pipewire *pipewire,
                                       const tb_pipewire_stream *stream)
{
    const char *const name = stream->target[0] != '\0' ? stream->target : pipewire->default_sink;
    const tb_pipewire_sink *const sink = tb_pipewire_find_sink(pipewire, name);

    return sink != NULL ? sink->description : name;
}

/* Tell every stream that follows the default (or all, with `all`) where it
 * plays now, or that it has nowhere to play. */
static void tb_pipewire_report_moves(tb_pipewire *pipewire, bool all)
{
    for (tb_pipewire_stream *stream = pipewire->streams; stream != NULL; stream = stream->next)
    {
        if (!all && stream->target[0] != '\0')
        {
            continue;
        }
        if (pipewire->sink_count == 0)
        {
            if (!stream->lost)
            {
                tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
            }
            stream->lost = true;
            continue;
        }
        stream->lost = false;
        tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED,
                               tb_pipewire_playing(pipewire, stream));
    }
}

/* ---------------------------------------------------------------------------
 * The daemon's events, on the loop's thread.
 * ------------------------------------------------------------------------- */

static void tb_pipewire_core_info(void *data, const struct tb_pw_core_info *info)
{
    tb_pipewire *const pipewire = data;
    const uint32_t rate = tb_spa_lookup_number(info->props, "default.clock.rate");

    if (rate >= TB_AUDIO_DEVICE_MIN_RATE && rate <= TB_AUDIO_DEVICE_MAX_RATE)
    {
        pipewire->clock_rate = rate;
    }
}

static void tb_pipewire_core_done(void *data, uint32_t id, int seq)
{
    tb_pipewire *const pipewire = data;

    if (id == TB_PW_ID_CORE && seq == pipewire->sync_seq)
    {
        pipewire->synced = true;
        pipewire->pw.thread_loop_signal(pipewire->loop, false);
    }
}

static void tb_pipewire_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    tb_pipewire *const pipewire = data;

    (void)seq;
    (void)res;
    (void)message;
    if (id == TB_PW_ID_CORE)
    {
        pipewire->failed = true;
        pipewire->pw.thread_loop_signal(pipewire->loop, false);
    }
}

static const struct tb_pw_core_events tb_pipewire_core_events = {.version = 0,
                                                                 .info = tb_pipewire_core_info,
                                                                 .done = tb_pipewire_core_done,
                                                                 .error = tb_pipewire_core_error};

static int tb_pipewire_metadata_property(void *data, uint32_t subject, const char *key,
                                         const char *type, const char *value)
{
    tb_pipewire *const pipewire = data;
    char name[TB_AUDIO_DEVICE_ID_CAPACITY] = {0};

    (void)subject;
    (void)type;
    if (key != NULL && !tb_str_equal(key, "default.audio.sink"))
    {
        return 0;
    }
    (void)tb_pipewire_default_name(value, name, sizeof(name));
    if (!tb_str_equal(name, pipewire->default_sink))
    {
        memcpy(pipewire->default_sink, name, sizeof(name));
        tb_pipewire_report_moves(pipewire, false);
    }
    return 0;
}

static const struct tb_pw_metadata_events tb_pipewire_metadata_events = {
    .version = 0, .property = tb_pipewire_metadata_property};

static void tb_pipewire_bind_metadata(tb_pipewire *pipewire, uint32_t id)
{
    const struct tb_spa_interface *const registry = pipewire->registry;
    const struct tb_pw_registry_methods *const methods = registry->cb.funcs;

    pipewire->metadata =
        methods->bind(registry->cb.data, id, TB_PW_TYPE_METADATA, TB_PW_VERSION_METADATA, 0);
    if (pipewire->metadata != NULL)
    {
        const struct tb_spa_interface *const metadata = pipewire->metadata;
        const struct tb_pw_metadata_methods *const metadata_methods = metadata->cb.funcs;
        (void)metadata_methods->add_listener(metadata->cb.data, &pipewire->metadata_hook,
                                             &tb_pipewire_metadata_events, pipewire);
    }
}

static void tb_pipewire_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                               uint32_t version, const struct tb_spa_dict *props)
{
    tb_pipewire *const pipewire = data;

    const char *const metadata_name = tb_spa_lookup(props, "metadata.name");

    (void)permissions;
    (void)version;
    if (tb_str_equal(type, TB_PW_TYPE_METADATA) && pipewire->metadata == NULL &&
        metadata_name != NULL && tb_str_equal(metadata_name, "default"))
    {
        tb_pipewire_bind_metadata(pipewire, id);
        return;
    }
    const char *const media_class = tb_spa_lookup(props, "media.class");
    if (!tb_str_equal(type, TB_PW_TYPE_NODE) || media_class == NULL ||
        !tb_str_equal(media_class, "Audio/Sink") || pipewire->sink_count == TB_PIPEWIRE_MAX_SINKS)
    {
        return;
    }
    tb_pipewire_sink *const sink = &pipewire->sinks[pipewire->sink_count];
    *sink = (tb_pipewire_sink){.id = id};
    tb_pipewire_copy(sink->name, sizeof(sink->name), tb_spa_lookup(props, "node.name"));
    tb_pipewire_copy(sink->description, sizeof(sink->description),
                     tb_spa_lookup(props, "node.description"));
    if (sink->description[0] == '\0')
    {
        memcpy(sink->description, sink->name, sizeof(sink->description) - 1);
    }
    sink->rate = tb_spa_lookup_number(props, "audio.rate");
    sink->channels = tb_spa_lookup_number(props, "audio.channels");
    pipewire->sink_count++;
    if (pipewire->sink_count == 1)
    {
        /* The first sink of a system that had none: lost streams play again. */
        for (tb_pipewire_stream *stream = pipewire->streams; stream != NULL; stream = stream->next)
        {
            if (stream->lost)
            {
                stream->lost = false;
                tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED,
                                       sink->description);
            }
        }
    }
}

static void tb_pipewire_global_remove(void *data, uint32_t id)
{
    tb_pipewire *const pipewire = data;
    char removed[TB_AUDIO_DEVICE_ID_CAPACITY] = {0};

    for (size_t index = 0; index < pipewire->sink_count; index++)
    {
        if (pipewire->sinks[index].id != id)
        {
            continue;
        }
        memcpy(removed, pipewire->sinks[index].name, sizeof(removed));
        pipewire->sinks[index] = pipewire->sinks[pipewire->sink_count - 1];
        pipewire->sink_count--;
        break;
    }
    if (removed[0] == '\0')
    {
        return;
    }
    /* A stream whose own sink went now follows the default. */
    for (tb_pipewire_stream *stream = pipewire->streams; stream != NULL; stream = stream->next)
    {
        if (tb_str_equal(stream->target, removed))
        {
            stream->target[0] = '\0';
            stream->lost = pipewire->sink_count == 0;
            if (stream->lost)
            {
                tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
            }
            else
            {
                tb_audio_device_report(stream->device, TB_AUDIO_DEVICE_EVENT_CHANGED,
                                       tb_pipewire_playing(pipewire, stream));
            }
        }
    }
    if (pipewire->sink_count == 0)
    {
        tb_pipewire_report_moves(pipewire, false);
    }
}

static const struct tb_pw_registry_events tb_pipewire_registry_events = {
    .version = 0, .global = tb_pipewire_global, .global_remove = tb_pipewire_global_remove};

/* ---------------------------------------------------------------------------
 * The system.
 * ------------------------------------------------------------------------- */

static void tb_pipewire_close(void *native)
{
    tb_pipewire *const pipewire = native;

    if (pipewire->loop != NULL)
    {
        pipewire->pw.thread_loop_stop(pipewire->loop);
    }
    if (pipewire->metadata != NULL)
    {
        pipewire->pw.proxy_destroy(pipewire->metadata);
    }
    if (pipewire->registry != NULL)
    {
        pipewire->pw.proxy_destroy(pipewire->registry);
    }
    if (pipewire->core != NULL)
    {
        (void)pipewire->pw.core_disconnect(pipewire->core);
    }
    if (pipewire->context != NULL)
    {
        pipewire->pw.context_destroy(pipewire->context);
    }
    if (pipewire->loop != NULL)
    {
        pipewire->pw.thread_loop_destroy(pipewire->loop);
    }
    if (pipewire->pw.deinit != NULL)
    {
        pipewire->pw.deinit();
    }
    tb_library_close(pipewire->library);
    tb_free(pipewire);
}

static void tb_pipewire_info(void *native, tb_audio_system_info *info)
{
    (void)native;
    memcpy(info->backend, "PipeWire", sizeof("PipeWire"));
    info->follows_default = true;
    info->reports_changes = true;
    info->knows_default = true;
}

static size_t tb_pipewire_list(void *native, tb_audio_device_info *devices, size_t capacity)
{
    tb_pipewire *const pipewire = native;
    size_t count = 0;

    tb_pipewire_lock(pipewire);
    count = pipewire->sink_count;
    for (size_t index = 0; index < count && index < capacity; index++)
    {
        const tb_pipewire_sink *const sink = &pipewire->sinks[index];
        memcpy(devices[index].id, sink->name, sizeof(devices[index].id));
        memcpy(devices[index].name, sink->description, sizeof(devices[index].name));
        devices[index].is_default = tb_str_equal(sink->name, pipewire->default_sink);
        devices[index].sample_rate = sink->rate;
        devices[index].channels = sink->channels;
    }
    tb_pipewire_unlock(pipewire);
    return count;
}

/* ---------------------------------------------------------------------------
 * Streams.
 * ------------------------------------------------------------------------- */

/* On PipeWire's data thread: fill the buffer the stream dequeues. */
static void tb_pipewire_process(void *data)
{
    tb_pipewire_stream *const stream = data;
    const tb_pw_functions *const pw = &stream->pipewire->pw;
    struct tb_pw_buffer *const buffer = pw->stream_dequeue_buffer(stream->stream);

    if (buffer == NULL)
    {
        return;
    }
    struct tb_spa_data *const target = &buffer->buffer->datas[0];
    const uint32_t stride = stream->channels * (uint32_t)sizeof(float);
    uint64_t frames = target->data != NULL ? target->maxsize / stride : 0;
    if (buffer->requested != 0 && buffer->requested < frames)
    {
        frames = buffer->requested;
    }
    tb_audio_device_render(stream->device, target->data, (size_t)frames, stream->channels);
    target->chunk->offset = 0;
    target->chunk->stride = (int32_t)stride;
    target->chunk->size = (uint32_t)frames * stride;
    (void)pw->stream_queue_buffer(stream->stream, buffer);
    tb_platform_atomic_u32_store(&stream->requested, (uint32_t)frames);
    tb_platform_atomic_u32_store(&stream->processed, 1u);
}

static void tb_pipewire_state_changed(void *data, int old, int state, const char *error)
{
    tb_pipewire_stream *const stream = data;

    (void)old;
    (void)error;
    stream->state = state;
    stream->pipewire->pw.thread_loop_signal(stream->pipewire->loop, false);
}

static const struct tb_pw_stream_events tb_pipewire_stream_events = {
    .version = 2, .state_changed = tb_pipewire_state_changed, .process = tb_pipewire_process};

static void tb_pipewire_unlink(tb_pipewire *pipewire, tb_pipewire_stream *stream)
{
    for (tb_pipewire_stream **link = &pipewire->streams; *link != NULL; link = &(*link)->next)
    {
        if (*link == stream)
        {
            *link = stream->next;
            return;
        }
    }
}

static void tb_pipewire_destroy_stream(tb_pipewire *pipewire, tb_pipewire_stream *stream)
{
    tb_pipewire_lock(pipewire);
    tb_pipewire_unlink(pipewire, stream);
    if (stream->stream != NULL)
    {
        pipewire->pw.stream_destroy(stream->stream);
    }
    tb_pipewire_unlock(pipewire);
    tb_free(stream);
}

/* Wait, holding the lock, for a stream to be linked or to fail. */
static bool tb_pipewire_wait_linked(tb_pipewire *pipewire, tb_pipewire_stream *stream)
{
    while (stream->state != TB_PW_STREAM_STATE_ERROR && stream->state < TB_PW_STREAM_STATE_PAUSED)
    {
        if (pipewire->pw.thread_loop_timed_wait(pipewire->loop, TB_PIPEWIRE_TIMEOUT_SECONDS) != 0)
        {
            return false;
        }
    }
    return stream->state != TB_PW_STREAM_STATE_ERROR;
}

/* The latency once the first buffer is asked for: the quantum, and the
 * delay PipeWire says lies between the stream and the speakers. */
static void tb_pipewire_measure(tb_pipewire *pipewire, tb_pipewire_stream *stream, uint32_t rate,
                                uint32_t period_frames)
{
    struct tb_pw_time time = {0};
    double delay_seconds = 0.0;

    for (uint32_t waited = 0; waited < TB_PIPEWIRE_FIRST_BUFFER_MILLISECONDS &&
                              tb_platform_atomic_u32_load(&stream->processed) == 0u;
         waited++)
    {
        tb_platform_sleep(1000000u);
    }
    uint32_t frames = tb_platform_atomic_u32_load(&stream->requested);
    if (frames == 0)
    {
        frames = period_frames;
    }
    tb_pipewire_lock(pipewire);
    if (pipewire->pw.stream_get_time_n(stream->stream, &time, sizeof(time)) == 0 &&
        time.rate_denom != 0 && time.delay > 0)
    {
        delay_seconds = (double)time.delay * (double)time.rate_num / (double)time.rate_denom;
    }
    tb_pipewire_unlock(pipewire);
    const tb_audio_device_latency latency =
        tb_audio_device_latency_of(frames, frames, rate, delay_seconds);
    tb_audio_device_set_latency(stream->device, &latency);
}

static tb_audio_device_result tb_pipewire_open_stream(void *native,
                                                      tb_audio_device_request *request,
                                                      tb_audio_device *device, void **opened,
                                                      tb_audio_device_error *error)
{
    tb_pipewire *const pipewire = native;
    tb_pipewire_stream *const stream = tb_calloc(1, sizeof(*stream));
    char latency_text[32] = {0};
    uint64_t pod[TB_PIPEWIRE_FORMAT_POD_CAPACITY / sizeof(uint64_t)] = {0};
    const void *params[1] = {pod};
    tb_audio_device_result result = TB_AUDIO_DEVICE_OK;

    stream->pipewire = pipewire;
    stream->device = device;
    tb_pipewire_lock(pipewire);
    if (request->format.sample_rate == 0)
    {
        request->format.sample_rate =
            pipewire->clock_rate != 0 ? pipewire->clock_rate : TB_PIPEWIRE_DEFAULT_RATE;
    }
    if (request->format.channels == 0)
    {
        request->format.channels = TB_PIPEWIRE_DEFAULT_CHANNELS;
    }
    stream->channels = request->format.channels;
    const uint32_t rate = request->format.sample_rate;
    const uint32_t period =
        request->period_frames != 0
            ? request->period_frames
            : (uint32_t)((double)rate * TB_PIPEWIRE_DEFAULT_PERIOD_SECONDS + 0.5);
    if (request->device_id[0] != '\0' &&
        tb_pipewire_find_sink(pipewire, request->device_id) == NULL)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_NOT_FOUND,
                                      "PipeWire: no sink is named '%s'", request->device_id);
        goto failed;
    }
    if (pipewire->sink_count == 0)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "PipeWire: no sink");
        goto failed;
    }
    tb_pipewire_copy(stream->target, sizeof(stream->target), request->device_id);
    (void)tb_fmt_buffer(latency_text, sizeof(latency_text), "%u/%u", period, rate);
    struct tb_pw_properties *const props = pipewire->pw.properties_new(
        "media.type", "Audio", "media.category", "Playback", "media.role", "Game", "media.name",
        request->stream_name, "node.latency", latency_text,
        stream->target[0] != '\0' ? "target.object" : NULL, stream->target, NULL);
    stream->stream = pipewire->pw.stream_new(pipewire->core, request->stream_name, props);
    if (stream->stream == NULL)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                      "PipeWire: the stream could not be made");
        goto failed;
    }
    pipewire->pw.stream_add_listener(stream->stream, &stream->hook, &tb_pipewire_stream_events,
                                     stream);
    (void)tb_pipewire_format_pod(rate, stream->channels, pod);
    if (pipewire->pw.stream_connect(stream->stream, TB_PW_DIRECTION_OUTPUT, TB_PW_ID_ANY,
                                    TB_PW_STREAM_FLAG_AUTOCONNECT | TB_PW_STREAM_FLAG_MAP_BUFFERS |
                                        TB_PW_STREAM_FLAG_RT_PROCESS,
                                    params, 1) < 0 ||
        !tb_pipewire_wait_linked(pipewire, stream))
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                      "PipeWire: the stream was not linked to a sink");
        goto failed;
    }
    stream->next = pipewire->streams;
    pipewire->streams = stream;
    tb_pipewire_unlock(pipewire);
    tb_pipewire_measure(pipewire, stream, rate, period);
    *opened = stream;
    return TB_AUDIO_DEVICE_OK;

failed:
    tb_pipewire_unlock(pipewire);
    tb_pipewire_destroy_stream(pipewire, stream);
    return result;
}

static void tb_pipewire_close_stream(void *native, void *opened)
{
    tb_pipewire_destroy_stream(native, opened);
}

/* Wait, holding the lock, for the daemon to answer everything asked so far. */
static bool tb_pipewire_roundtrip(tb_pipewire *pipewire)
{
    const struct tb_spa_interface *const core = pipewire->core;
    const struct tb_pw_core_methods *const methods = core->cb.funcs;

    pipewire->synced = false;
    pipewire->sync_seq = methods->sync(core->cb.data, TB_PW_ID_CORE, 0);
    while (!pipewire->synced && !pipewire->failed)
    {
        if (pipewire->pw.thread_loop_timed_wait(pipewire->loop, TB_PIPEWIRE_TIMEOUT_SECONDS) != 0)
        {
            return false;
        }
    }
    return !pipewire->failed;
}

/* Listen to the core and the registry, and wait for what the registry has. */
static bool tb_pipewire_listen(tb_pipewire *pipewire)
{
    const struct tb_spa_interface *const core = pipewire->core;
    const struct tb_pw_core_methods *const methods = core->cb.funcs;

    (void)methods->add_listener(core->cb.data, &pipewire->core_hook, &tb_pipewire_core_events,
                                pipewire);
    pipewire->registry = methods->get_registry(core->cb.data, TB_PW_VERSION_REGISTRY, 0);
    if (pipewire->registry == NULL)
    {
        return false;
    }
    const struct tb_spa_interface *const registry = pipewire->registry;
    const struct tb_pw_registry_methods *const registry_methods = registry->cb.funcs;
    (void)registry_methods->add_listener(registry->cb.data, &pipewire->registry_hook,
                                         &tb_pipewire_registry_events, pipewire);
    /* Twice: the registry's globals, then the metadata bound on seeing it. */
    return tb_pipewire_roundtrip(pipewire) && tb_pipewire_roundtrip(pipewire);
}
/* coverage-on */

static const tb_audio_system_ops tb_pipewire_ops = {tb_pipewire_close, tb_pipewire_info,
                                                    tb_pipewire_list, tb_pipewire_open_stream,
                                                    tb_pipewire_close_stream};

tb_audio_device_result tb_pipewire_open(const tb_audio_system_ops **ops, void **native,
                                        tb_audio_device_error *error)
{
    tb_pipewire *const pipewire = tb_calloc(1, sizeof(*pipewire));
    tb_pw_functions *const pw = &pipewire->pw;
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    const tb_library_binding bindings[] = {
        {"pw_init", (tb_library_function *)&pw->init, true},
        {"pw_deinit", (tb_library_function *)&pw->deinit, true},
        {"pw_thread_loop_new", (tb_library_function *)&pw->thread_loop_new, true},
        {"pw_thread_loop_destroy", (tb_library_function *)&pw->thread_loop_destroy, true},
        {"pw_thread_loop_start", (tb_library_function *)&pw->thread_loop_start, true},
        {"pw_thread_loop_stop", (tb_library_function *)&pw->thread_loop_stop, true},
        {"pw_thread_loop_lock", (tb_library_function *)&pw->thread_loop_lock, true},
        {"pw_thread_loop_unlock", (tb_library_function *)&pw->thread_loop_unlock, true},
        {"pw_thread_loop_timed_wait", (tb_library_function *)&pw->thread_loop_timed_wait, true},
        {"pw_thread_loop_signal", (tb_library_function *)&pw->thread_loop_signal, true},
        {"pw_thread_loop_get_loop", (tb_library_function *)&pw->thread_loop_get_loop, true},
        {"pw_context_new", (tb_library_function *)&pw->context_new, true},
        {"pw_context_destroy", (tb_library_function *)&pw->context_destroy, true},
        {"pw_context_connect", (tb_library_function *)&pw->context_connect, true},
        {"pw_core_disconnect", (tb_library_function *)&pw->core_disconnect, true},
        {"pw_proxy_destroy", (tb_library_function *)&pw->proxy_destroy, true},
        {"pw_properties_new", (tb_library_function *)&pw->properties_new, true},
        {"pw_stream_new", (tb_library_function *)&pw->stream_new, true},
        {"pw_stream_add_listener", (tb_library_function *)&pw->stream_add_listener, true},
        {"pw_stream_connect", (tb_library_function *)&pw->stream_connect, true},
        {"pw_stream_destroy", (tb_library_function *)&pw->stream_destroy, true},
        {"pw_stream_dequeue_buffer", (tb_library_function *)&pw->stream_dequeue_buffer, true},
        {"pw_stream_queue_buffer", (tb_library_function *)&pw->stream_queue_buffer, true},
        {"pw_stream_get_time_n", (tb_library_function *)&pw->stream_get_time_n, true},
    };
    tb_audio_device_result result = TB_AUDIO_DEVICE_OK;

    if (tb_library_open("libpipewire-0.3.so.0", TB_LIBRARY_SEARCH_SYSTEM, &pipewire->library,
                        &library_error) != TB_LIBRARY_OK)
    {
        tb_free(pipewire);
        return tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE, "PipeWire: %s",
                                    library_error.message);
    }
    /* coverage-off: needs PipeWire's library, which CI's runners do not install */
    if (tb_library_bind(pipewire->library, bindings, sizeof(bindings) / sizeof(bindings[0]),
                        &library_error) != TB_LIBRARY_OK)
    {
        pw->deinit = NULL;
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                      "PipeWire: older than 0.3.50: %s", library_error.message);
        goto failed;
    }
    pw->init(NULL, NULL);
    pipewire->loop = pw->thread_loop_new("badger-pipewire", NULL);
    if (pipewire->loop == NULL)
    {
        result =
            tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR, "PipeWire: no thread loop");
        goto failed;
    }
    pipewire->context = pw->context_new(pw->thread_loop_get_loop(pipewire->loop), NULL, 0);
    if (pipewire->context == NULL)
    {
        /* Its configuration is missing, as in a container without it. */
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                      "PipeWire: no context: is its configuration installed?");
        goto failed;
    }
    pipewire->core = pw->context_connect(pipewire->context, NULL, 0);
    if (pipewire->core == NULL)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                      "PipeWire: no daemon is running");
        goto failed;
    }
    if (pw->thread_loop_start(pipewire->loop) != 0)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_SYSTEM_ERROR,
                                      "PipeWire: the thread loop did not start");
        goto failed;
    }
    tb_pipewire_lock(pipewire);
    const bool listening = tb_pipewire_listen(pipewire);
    tb_pipewire_unlock(pipewire);
    if (!listening)
    {
        result = tb_audio_device_fail(error, TB_AUDIO_DEVICE_UNAVAILABLE,
                                      "PipeWire: the daemon did not answer");
        goto failed;
    }
    *ops = &tb_pipewire_ops;
    *native = pipewire;
    return TB_AUDIO_DEVICE_OK;

failed:
    tb_pipewire_close(pipewire);
    return result;
    /* coverage-on */
}
