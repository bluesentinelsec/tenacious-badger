/* The platform's audio devices, where the machine has any: the system
 * opens and describes itself, lists its outputs, and a device opened on
 * the default output asks for samples on its own thread, in the format it
 * was opened in, only while it is started. The render function writes
 * silence, so the cases make no sound. A machine without audio, as CI's
 * runners are, skips them. */
#include "audio_display_state.h"

#include "badger/audio_device.h"

#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

#include "tb_test.h"

/* How long a device has to ask for samples: generous, for a loaded machine. */
#define WAIT_NANOSECONDS 3000000000ull

/* A stopped device is watched this long for a call it must not make. */
#define QUIET_NANOSECONDS 100000000ull

typedef struct listener
{
    tb_platform_atomic_size calls;
    tb_platform_atomic_size frames;
    tb_platform_atomic_u32 thread_differs;
    uint64_t caller_thread;
    uint32_t channels;
    tb_platform_atomic_u32 wrong_channels;
} listener;

static void render_silence(void *context, float *samples, size_t frame_count, uint32_t channels)
{
    listener *const heard = context;

    (void)samples; /* Silent already: the device clears it. */
    if (channels != heard->channels)
    {
        tb_platform_atomic_u32_store(&heard->wrong_channels, 1u);
    }
    if (tb_thread_current_id() != heard->caller_thread)
    {
        tb_platform_atomic_u32_store(&heard->thread_differs, 1u);
    }
    (void)tb_platform_atomic_size_add(&heard->frames, frame_count);
    (void)tb_platform_atomic_size_add(&heard->calls, 1);
}

static tb_audio_system *open_system_or_skip(void)
{
    tb_audio_system *system = NULL;
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
    const tb_audio_device_result result = tb_audio_system_open(&system, &error);

    if (result == TB_AUDIO_DEVICE_UNAVAILABLE)
    {
        TB_TEST_SKIP("no audio: %s", error.message);
    }
    TB_TEST_REQUIRE_INT(result, ==, TB_AUDIO_DEVICE_OK);
    return system;
}

static bool wait_for_calls(listener *heard, size_t count)
{
    const uint64_t deadline = tb_platform_clock_nanoseconds() + WAIT_NANOSECONDS;

    while (tb_platform_atomic_size_load(&heard->calls) < count)
    {
        if (tb_platform_clock_nanoseconds() > deadline)
        {
            return false;
        }
        tb_platform_sleep(1000000u);
    }
    return true;
}

/* The system names its backend, and every device it lists has an
 * identifier and a name; at most one is the default. */
TB_TEST(platform_audio_device, the_system_lists_its_outputs)
{
    tb_audio_system *const system = open_system_or_skip();
    tb_audio_system_info info = {0};
    tb_audio_device_info devices[16] = {0};

    tb_audio_system_get_info(system, &info);
    TB_TEST_EXPECT(tb_str_length(info.backend) > 0);
    const size_t count = tb_audio_system_list_devices(system, NULL, 0);
    const size_t listed = tb_audio_system_list_devices(system, devices, TB_TEST_COUNT_OF(devices));
    TB_TEST_EXPECT_UINT(listed, ==, count);
    size_t defaults = 0;
    for (size_t index = 0; index < listed && index < TB_TEST_COUNT_OF(devices); index++)
    {
        TB_TEST_CONTEXT("device %zu: '%s' (%s)", index, devices[index].name, devices[index].id);
        TB_TEST_EXPECT(tb_str_length(devices[index].id) > 0);
        defaults += devices[index].is_default ? 1u : 0u;
    }
    TB_TEST_EXPECT_UINT(defaults, <=, 1);
    tb_audio_system_close(system);
}

/* The default output asks for samples on a thread of its own, in the
 * format asked for, and stops asking when stopped. */
TB_TEST(platform_audio_device, the_default_output_asks_for_samples)
{
    tb_audio_system *const system = open_system_or_skip();
    listener heard = {.caller_thread = tb_thread_current_id(), .channels = 2};
    tb_audio_device *device = NULL;
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
    tb_audio_format format = {0};
    tb_audio_device_latency latency = {0};
    const tb_audio_device_desc desc = {.format = {.sample_rate = 48000, .channels = 2},
                                       .stream_name = "badger tests",
                                       .render = render_silence,
                                       .context = &heard};

    const tb_audio_device_result result = tb_audio_device_open(system, &desc, &device, &error);
    if (result == TB_AUDIO_DEVICE_UNAVAILABLE)
    {
        tb_audio_system_close(system);
        TB_TEST_SKIP("no output device: %s", error.message);
    }
    TB_TEST_REQUIRE_INT(result, ==, TB_AUDIO_DEVICE_OK);
    tb_audio_device_get_format(device, &format);
    TB_TEST_EXPECT_UINT(format.sample_rate, ==, 48000);
    TB_TEST_EXPECT_UINT(format.channels, ==, 2);

    tb_audio_device_start(device);
    const bool delivered = wait_for_calls(&heard, 3);
    if (!delivered)
    {
        tb_audio_system_info info = {0};
        tb_audio_system_get_info(system, &info);
        const tb_test_audio_display display = tb_test_audio_display_state();
        const char *hint = "check the default output route";
        if (display == TB_TEST_AUDIO_DISPLAY_ASLEEP)
        {
            hint = "main display asleep; repeat awake (see docs/audio.md)";
        }
        else if (display == TB_TEST_AUDIO_DISPLAY_AWAKE)
        {
            hint = "main display awake; check the audio route and callback delivery";
        }
        TB_TEST_CONTEXT("%s default output opened at %u Hz, %u channels, and requested %zu "
                        "callbacks in 3 s; %s",
                        info.backend, format.sample_rate, format.channels,
                        tb_platform_atomic_size_load(&heard.calls), hint);
    }
    TB_TEST_EXPECT(delivered);
    tb_audio_device_stop(device);
    const size_t calls = tb_platform_atomic_size_load(&heard.calls);
    tb_platform_sleep(QUIET_NANOSECONDS);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&heard.calls), ==, calls);
    if (calls > 0)
    {
        TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&heard.thread_differs), ==, 1);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&heard.wrong_channels), ==, 0);
    }

    tb_audio_device_get_latency(device, &latency);
    TB_TEST_CONTEXT("latency: period %u, buffer %u, %.1f ms", latency.period_frames,
                    latency.buffer_frames, latency.seconds * 1000.0);
    TB_TEST_EXPECT(latency.buffer_frames > 0);
    TB_TEST_EXPECT(latency.seconds > 0.0 && latency.seconds < 1.0);
    tb_audio_device_close(device);
    tb_audio_system_close(system);
}

/* A device of the system's own rate and channels is opened at what the
 * system says, and an identifier no device has is not found. */
TB_TEST(platform_audio_device, the_device_s_own_format_and_unknown_devices)
{
    tb_audio_system *const system = open_system_or_skip();
    listener heard = {0};
    tb_audio_device *device = NULL;
    tb_audio_format format = {0};
    tb_audio_device_desc desc = {.render = render_silence, .context = &heard};

    const tb_audio_device_result result = tb_audio_device_open(system, &desc, &device, NULL);
    if (result == TB_AUDIO_DEVICE_UNAVAILABLE)
    {
        tb_audio_system_close(system);
        TB_TEST_SKIP("no output device");
    }
    TB_TEST_REQUIRE_INT(result, ==, TB_AUDIO_DEVICE_OK);
    tb_audio_device_get_format(device, &format);
    TB_TEST_EXPECT(format.sample_rate >= TB_AUDIO_DEVICE_MIN_RATE);
    TB_TEST_EXPECT(format.channels >= 1 && format.channels <= TB_AUDIO_DEVICE_MAX_CHANNELS);
    tb_audio_device_close(device);

    desc.device_id = "no such device, anywhere";
    TB_TEST_EXPECT_INT(tb_audio_device_open(system, &desc, &device, NULL), ==,
                       TB_AUDIO_DEVICE_NOT_FOUND);
    TB_TEST_EXPECT_POINTER(device, ==, NULL);
    tb_audio_system_close(system);
}
