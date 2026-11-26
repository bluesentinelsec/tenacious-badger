/* The shared front of the audio device interface, over a backend of the
 * test's own: the render function is called only between start and stop,
 * always with silence to write into, and never after stop returns, even
 * from a thread that renders all the while; formats are resolved by the
 * backend and kept; events queue in order and drop the oldest; and the
 * helpers that name, lay out and time a stream. */
#include "platform/shared/audio_device.h"

#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <string.h>

#define FAKE_RATE 48000u
#define FAKE_CHANNELS 2u
#define FAKE_FRAMES 64u

/* A stream of the fake backend. With `threaded`, a thread of its own
 * renders until the stream closes, as a real backend's does. */
typedef struct fake_stream
{
    tb_audio_device *device;
    bool threaded;
    tb_thread thread;
    tb_platform_atomic_u32 quit;
    float buffer[FAKE_FRAMES * FAKE_CHANNELS];
} fake_stream;

typedef struct fake_backend
{
    bool threaded;
    tb_audio_device_result open_result;
    tb_audio_device_request last_request;
    char last_device_id[TB_AUDIO_DEVICE_ID_CAPACITY];
    fake_stream *stream;
    size_t streams_closed;
    bool closed;
} fake_backend;

/* What the test's render function saw. */
typedef struct fake_listener
{
    tb_platform_atomic_size calls;
    tb_platform_atomic_u32 heard_noise; /* A buffer that was not silent on entry. */
    size_t last_frames;
    uint32_t last_channels;
} fake_listener;

static void fake_render(void *context, float *samples, size_t frame_count, uint32_t channels)
{
    fake_listener *const listener = context;

    for (size_t index = 0; index < frame_count * channels; index++)
    {
        if (samples[index] != 0.0f)
        {
            tb_platform_atomic_u32_store(&listener->heard_noise, 1u);
        }
        samples[index] = 0.25f;
    }
    listener->last_frames = frame_count;
    listener->last_channels = channels;
    (void)tb_platform_atomic_size_add(&listener->calls, 1);
}

static void fake_stream_main(void *argument)
{
    fake_stream *const stream = argument;

    while (tb_platform_atomic_u32_load(&stream->quit) == 0u)
    {
        /* Noise before each render, which the front must clear. */
        for (size_t index = 0; index < FAKE_FRAMES * FAKE_CHANNELS; index++)
        {
            stream->buffer[index] = 1.0f;
        }
        tb_audio_device_render(stream->device, stream->buffer, FAKE_FRAMES, FAKE_CHANNELS);
        tb_thread_yield();
    }
}

static void fake_close(void *native)
{
    fake_backend *const backend = native;

    backend->closed = true;
}

static void fake_info(void *native, tb_audio_system_info *info)
{
    (void)native;
    memcpy(info->backend, "fake", sizeof("fake"));
    info->follows_default = true;
}

static size_t fake_list(void *native, tb_audio_device_info *devices, size_t capacity)
{
    static const char *const names[] = {"Speakers", "Headphones", "Monitor"};

    (void)native;
    for (size_t index = 0; index < capacity && index < 3; index++)
    {
        tb_audio_device_copy_name(devices[index].name, sizeof(devices[index].name), names[index],
                                  tb_str_length(names[index]));
        devices[index].is_default = index == 0;
    }
    return 3;
}

static tb_audio_device_result fake_open(void *native, tb_audio_device_request *request,
                                        tb_audio_device *device, void **opened,
                                        tb_audio_device_error *error)
{
    fake_backend *const backend = native;
    fake_stream *stream = NULL;
    const tb_audio_device_latency latency =
        tb_audio_device_latency_of(FAKE_FRAMES, FAKE_FRAMES * 2u, FAKE_RATE, 0.0);

    backend->last_request = *request;
    tb_audio_device_copy_name(backend->last_device_id, sizeof(backend->last_device_id),
                              request->device_id, tb_str_length(request->device_id));
    if (backend->open_result != TB_AUDIO_DEVICE_OK)
    {
        return tb_audio_device_fail(error, backend->open_result, "fake: refused");
    }
    if (request->format.sample_rate == 0)
    {
        request->format.sample_rate = FAKE_RATE;
    }
    if (request->format.channels == 0)
    {
        request->format.channels = FAKE_CHANNELS;
    }
    stream = tb_calloc(1, sizeof(*stream));
    stream->device = device;
    stream->threaded = backend->threaded;
    tb_audio_device_set_latency(device, &latency);
    if (stream->threaded)
    {
        tb_thread_create(&stream->thread, fake_stream_main, stream);
    }
    backend->stream = stream;
    *opened = stream;
    return TB_AUDIO_DEVICE_OK;
}

static void fake_close_stream(void *native, void *opened)
{
    fake_backend *const backend = native;
    fake_stream *const stream = opened;

    if (stream->threaded)
    {
        tb_platform_atomic_u32_store(&stream->quit, 1u);
        tb_thread_join(&stream->thread);
    }
    backend->streams_closed++;
    backend->stream = NULL;
    tb_free(stream);
}

static const tb_audio_system_ops fake_ops = {.close = fake_close,
                                             .info = fake_info,
                                             .list = fake_list,
                                             .open = fake_open,
                                             .close_stream = fake_close_stream};

static tb_audio_device *open_device(tb_audio_system *system, fake_listener *listener,
                                    const tb_audio_format *format)
{
    tb_audio_device *device = NULL;
    const tb_audio_device_desc desc = {
        .format = *format, .render = fake_render, .context = listener};

    TB_TEST_REQUIRE_INT(tb_audio_device_open(system, &desc, &device, NULL), ==,
                        TB_AUDIO_DEVICE_OK);
    TB_TEST_REQUIRE(device != NULL);
    return device;
}

/* A stopped device renders silence without calling the caller; a started
 * one calls it with a buffer it has cleared, whatever the backend left in
 * it, and passes on what it wrote. */
TB_TEST(platform_audio_device_front, renders_only_while_started)
{
    fake_backend backend = {0};
    fake_listener listener = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    const tb_audio_format format = {0};
    tb_audio_device *const device = open_device(system, &listener, &format);
    float buffer[FAKE_FRAMES * FAKE_CHANNELS] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(buffer); index++)
    {
        buffer[index] = 1.0f;
    }
    tb_audio_device_render(device, buffer, FAKE_FRAMES, FAKE_CHANNELS);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&listener.calls), ==, 0);
    TB_TEST_EXPECT_FLOAT(buffer[0], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(buffer[TB_TEST_COUNT_OF(buffer) - 1], ==, 0.0f);

    tb_audio_device_start(device);
    tb_audio_device_start(device);
    buffer[3] = -1.0f;
    tb_audio_device_render(device, buffer, FAKE_FRAMES, FAKE_CHANNELS);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&listener.calls), ==, 1);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&listener.heard_noise), ==, 0);
    TB_TEST_EXPECT_UINT(listener.last_frames, ==, FAKE_FRAMES);
    TB_TEST_EXPECT_UINT(listener.last_channels, ==, FAKE_CHANNELS);
    TB_TEST_EXPECT_FLOAT(buffer[3], ==, 0.25f);

    /* No frames: nothing to call it for. */
    tb_audio_device_render(device, NULL, 0, FAKE_CHANNELS);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&listener.calls), ==, 1);

    tb_audio_device_stop(device);
    tb_audio_device_stop(device);
    tb_audio_device_render(device, buffer, FAKE_FRAMES, FAKE_CHANNELS);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&listener.calls), ==, 1);
    TB_TEST_EXPECT_FLOAT(buffer[3], ==, 0.0f);

    tb_audio_device_close(device);
    TB_TEST_EXPECT_UINT(backend.streams_closed, ==, 1);
    tb_audio_system_close(system);
    TB_TEST_EXPECT(backend.closed);
}

/* Stop's promise holds against a thread that renders without pause: once
 * it returns, the count of calls stays where it was. Started and stopped
 * many times over, so that a stop lands in every part of a render. */
TB_TEST(platform_audio_device_front, stop_waits_for_a_render_in_progress)
{
    fake_backend backend = {.threaded = true};
    fake_listener listener = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    const tb_audio_format format = {.sample_rate = 44100, .channels = 2};
    tb_audio_device *const device = open_device(system, &listener, &format);

    for (size_t round = 0; round < 200; round++)
    {
        const size_t before = tb_platform_atomic_size_load(&listener.calls);
        tb_audio_device_start(device);
        while (tb_platform_atomic_size_load(&listener.calls) == before)
        {
            tb_thread_yield();
        }
        tb_audio_device_stop(device);
        const size_t stopped = tb_platform_atomic_size_load(&listener.calls);
        for (size_t spin = 0; spin < 50; spin++)
        {
            tb_thread_yield();
        }
        TB_TEST_REQUIRE_UINT(tb_platform_atomic_size_load(&listener.calls), ==, stopped);
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&listener.heard_noise), ==, 0);
    tb_audio_device_close(device);
    tb_audio_system_close(system);
}

/* What the caller asks for reaches the backend as it asked, with the
 * defaults filled in; what the backend opens is the format, and its
 * latency is the device's. */
TB_TEST(platform_audio_device_front, the_backend_resolves_the_format)
{
    fake_backend backend = {0};
    fake_listener listener = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    tb_audio_device *device = NULL;
    tb_audio_format format = {0};
    tb_audio_device_latency latency = {0};
    const tb_audio_device_desc desc = {.device_id = "speakers",
                                       .format = {.sample_rate = 0, .channels = 6},
                                       .period_frames = 128,
                                       .stream_name = "Burrow",
                                       .render = fake_render,
                                       .context = &listener};

    TB_TEST_REQUIRE_INT(tb_audio_device_open(system, &desc, &device, NULL), ==,
                        TB_AUDIO_DEVICE_OK);
    TB_TEST_EXPECT_STRING(backend.last_device_id, ==, "speakers");
    TB_TEST_EXPECT_UINT(backend.last_request.period_frames, ==, 128);
    TB_TEST_EXPECT_STRING(backend.last_request.stream_name, ==, "Burrow");
    TB_TEST_EXPECT_UINT(backend.last_request.format.channels, ==, 6);
    tb_audio_device_get_format(device, &format);
    TB_TEST_EXPECT_UINT(format.sample_rate, ==, FAKE_RATE);
    TB_TEST_EXPECT_UINT(format.channels, ==, 6);
    tb_audio_device_get_latency(device, &latency);
    TB_TEST_EXPECT_UINT(latency.period_frames, ==, FAKE_FRAMES);
    TB_TEST_EXPECT_UINT(latency.buffer_frames, ==, FAKE_FRAMES * 2u);
    TB_TEST_EXPECT_FLOAT_NEAR(latency.seconds, (double)(FAKE_FRAMES * 2u) / FAKE_RATE, 1e-9);
    tb_audio_device_close(device);

    /* No identifier and no name are the default device, and "badger". */
    format = (tb_audio_format){0};
    device = open_device(system, &listener, &format);
    TB_TEST_EXPECT_STRING(backend.last_device_id, ==, "");
    TB_TEST_EXPECT_STRING(backend.last_request.stream_name, ==, "badger");
    tb_audio_device_get_format(device, &format);
    TB_TEST_EXPECT_UINT(format.channels, ==, FAKE_CHANNELS);
    tb_audio_device_close(device);
    tb_audio_system_close(system);
}

/* A backend's refusal comes back as it gave it, with nothing left open. */
TB_TEST(platform_audio_device_front, a_refusal_is_reported)
{
    fake_backend backend = {.open_result = TB_AUDIO_DEVICE_NOT_FOUND};
    fake_listener listener = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    tb_audio_device *device = NULL;
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
    const tb_audio_device_desc desc = {.render = fake_render, .context = &listener};

    TB_TEST_EXPECT_INT(tb_audio_device_open(system, &desc, &device, &error), ==,
                       TB_AUDIO_DEVICE_NOT_FOUND);
    TB_TEST_EXPECT_POINTER(device, ==, NULL);
    TB_TEST_EXPECT_INT(error.result, ==, TB_AUDIO_DEVICE_NOT_FOUND);
    TB_TEST_EXPECT_STRING(error.message, ==, "fake: refused");
    backend.open_result = TB_AUDIO_DEVICE_SYSTEM_ERROR;
    TB_TEST_EXPECT_INT(tb_audio_device_open(system, &desc, &device, NULL), ==,
                       TB_AUDIO_DEVICE_SYSTEM_ERROR);
    tb_audio_system_close(system);
    tb_audio_device_close(NULL);
    tb_audio_system_close(NULL);
}

/* Events come out in the order they went in; past the queue's room, the
 * oldest go. A name is cut to fit, at a character. */
TB_TEST(platform_audio_device_front, events_queue_in_order)
{
    fake_backend backend = {0};
    fake_listener listener = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    const tb_audio_format format = {0};
    tb_audio_device *const device = open_device(system, &listener, &format);
    tb_audio_device_event event = {0};
    char long_name[300] = {0};

    TB_TEST_EXPECT(!tb_audio_device_next_event(device, &event));
    tb_audio_device_report(device, TB_AUDIO_DEVICE_EVENT_LOST, NULL);
    tb_audio_device_report(device, TB_AUDIO_DEVICE_EVENT_CHANGED, "Headphones");
    TB_TEST_REQUIRE(tb_audio_device_next_event(device, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_AUDIO_DEVICE_EVENT_LOST);
    TB_TEST_EXPECT_STRING(event.device_name, ==, "");
    TB_TEST_REQUIRE(tb_audio_device_next_event(device, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_AUDIO_DEVICE_EVENT_CHANGED);
    TB_TEST_EXPECT_STRING(event.device_name, ==, "Headphones");
    TB_TEST_EXPECT(!tb_audio_device_next_event(device, &event));

    for (size_t index = 0; index < 20; index++)
    {
        tb_audio_device_report(device, index < 19 ? TB_AUDIO_DEVICE_EVENT_LOST
                                                   : TB_AUDIO_DEVICE_EVENT_CHANGED,
                               NULL);
    }
    size_t taken = 0;
    while (tb_audio_device_next_event(device, &event))
    {
        taken++;
    }
    TB_TEST_EXPECT_UINT(taken, ==, 16);
    TB_TEST_EXPECT_INT(event.type, ==, TB_AUDIO_DEVICE_EVENT_CHANGED);

    for (size_t index = 0; index + 2 < sizeof(long_name); index += 2)
    {
        memcpy(long_name + index, "\xc3\xa9", 2); /* é, two bytes */
    }
    tb_audio_device_report(device, TB_AUDIO_DEVICE_EVENT_CHANGED, long_name);
    TB_TEST_REQUIRE(tb_audio_device_next_event(device, &event));
    TB_TEST_EXPECT_UINT(tb_str_length(event.device_name), ==, TB_AUDIO_DEVICE_NAME_CAPACITY - 2);
    tb_audio_device_close(device);
    tb_audio_system_close(system);
}

/* The system says what its backend says, and lists what it lists, within
 * the room it is given; with none, it counts. */
TB_TEST(platform_audio_device_front, the_system_describes_and_lists)
{
    fake_backend backend = {0};
    tb_audio_system *const system = tb_audio_system_make(&fake_ops, &backend);
    tb_audio_system_info info = {0};
    tb_audio_device_info devices[2] = {0};

    tb_audio_system_get_info(system, &info);
    TB_TEST_EXPECT_STRING(info.backend, ==, "fake");
    TB_TEST_EXPECT(info.follows_default);
    TB_TEST_EXPECT_UINT(tb_audio_system_list_devices(system, NULL, 0), ==, 3);
    TB_TEST_EXPECT_UINT(tb_audio_system_list_devices(system, devices, 2), ==, 3);
    TB_TEST_EXPECT_STRING(devices[0].name, ==, "Speakers");
    TB_TEST_EXPECT(devices[0].is_default);
    TB_TEST_EXPECT_STRING(devices[1].name, ==, "Headphones");
    TB_TEST_EXPECT(!devices[1].is_default);
    tb_audio_system_close(system);
}

/* Names that are not UTF-8 have '?' for each bad byte; the layouts are
 * WAVE_FORMAT_EXTENSIBLE's; latency counts the buffer at the rate and adds
 * the device's own. */
TB_TEST(platform_audio_device_front, helpers)
{
    char name[8] = {0};
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;

    tb_audio_device_copy_name(name, sizeof(name), "a\xff" "b", 3);
    TB_TEST_EXPECT_STRING(name, ==, "a?b");
    tb_audio_device_copy_name(name, sizeof(name), "abcdefghij", 10);
    TB_TEST_EXPECT_STRING(name, ==, "abcdefg");
    tb_audio_device_copy_name(name, sizeof(name), NULL, 0);
    TB_TEST_EXPECT_STRING(name, ==, "");

    TB_TEST_EXPECT_HEX(tb_audio_device_channel_mask(1), ==, 0x4u);
    TB_TEST_EXPECT_HEX(tb_audio_device_channel_mask(2), ==, 0x3u);
    TB_TEST_EXPECT_HEX(tb_audio_device_channel_mask(6), ==, 0x3fu);
    TB_TEST_EXPECT_HEX(tb_audio_device_channel_mask(8), ==, 0x63fu);
    TB_TEST_EXPECT_HEX(tb_audio_device_channel_mask(9), ==, 0u);

    const tb_audio_device_latency latency = tb_audio_device_latency_of(480, 960, 48000, 0.005);
    TB_TEST_EXPECT_UINT(latency.period_frames, ==, 480);
    TB_TEST_EXPECT_FLOAT_NEAR(latency.seconds, 0.025, 1e-9);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_audio_device_latency_of(1, 1, 0, 0.5).seconds, 0.5, 1e-9);

    TB_TEST_EXPECT_INT(tb_audio_device_fail(NULL, TB_AUDIO_DEVICE_UNAVAILABLE, "x"), ==,
                       TB_AUDIO_DEVICE_UNAVAILABLE);
    TB_TEST_EXPECT_INT(
        tb_audio_device_fail(&error, TB_AUDIO_DEVICE_SYSTEM_ERROR, "code %d", 7), ==,
        TB_AUDIO_DEVICE_SYSTEM_ERROR);
    TB_TEST_EXPECT_STRING(error.message, ==, "code 7");

    TB_TEST_EXPECT_STRING(tb_audio_device_result_name(TB_AUDIO_DEVICE_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_audio_device_result_name(TB_AUDIO_DEVICE_UNAVAILABLE), ==,
                          "unavailable");
    TB_TEST_EXPECT_STRING(tb_audio_device_result_name(TB_AUDIO_DEVICE_NOT_FOUND), ==,
                          "not found");
    TB_TEST_EXPECT_STRING(tb_audio_device_result_name(TB_AUDIO_DEVICE_SYSTEM_ERROR), ==,
                          "system error");
    TB_TEST_EXPECT_STRING(tb_audio_device_result_name((tb_audio_device_result)99), ==,
                          "unknown result");
}
