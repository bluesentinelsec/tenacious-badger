/* Each Linux audio backend by itself, where the machine has it: PipeWire
 * where a daemon runs, and ALSA where a sound card is, though PipeWire
 * would be chosen first. Each lists its outputs, opens the default, is
 * asked for samples on its own thread while started, and refuses a name no
 * device has. The render function writes silence, so the cases make no
 * sound. A machine without either, as CI's runners are, skips them. */
#include "platform/linux/audio_linux.h"

#include "badger/platform.h"
#include "badger/str.h"

#include "tb_test.h"

#define WAIT_NANOSECONDS 3000000000ull

typedef tb_audio_device_result open_function(const tb_audio_system_ops **ops, void **native,
                                             tb_audio_device_error *error);

static void count_calls(void *context, float *samples, size_t frame_count, uint32_t channels)
{
    tb_platform_atomic_size *const calls = context;

    (void)samples;
    (void)frame_count;
    (void)channels;
    (void)tb_platform_atomic_size_add(calls, 1);
}

static void exercise(open_function *open_backend, const char *backend)
{
    const tb_audio_system_ops *ops = NULL;
    void *native = NULL;
    tb_audio_device_error error = TB_AUDIO_DEVICE_ERROR_INIT;
    tb_platform_atomic_size calls = {0};
    tb_audio_device *device = NULL;
    tb_audio_system_info info = {0};
    tb_audio_device_info devices[8] = {0};
    tb_audio_device_latency latency = {0};
    tb_audio_device_desc desc = {
        .format = {.sample_rate = 48000, .channels = 2}, .render = count_calls, .context = &calls};

    if (open_backend(&ops, &native, &error) != TB_AUDIO_DEVICE_OK)
    {
        TB_TEST_SKIP("no %s: %s", backend, error.message);
    }
    tb_audio_system *const system = tb_audio_system_make(ops, native);
    tb_audio_system_get_info(system, &info);
    TB_TEST_EXPECT_STRING(info.backend, ==, backend);
    const size_t count = tb_audio_system_list_devices(system, devices, TB_TEST_COUNT_OF(devices));
    for (size_t index = 0; index < count && index < TB_TEST_COUNT_OF(devices); index++)
    {
        TB_TEST_EXPECT(tb_str_length(devices[index].id) > 0);
    }
    if (tb_audio_device_open(system, &desc, &device, &error) != TB_AUDIO_DEVICE_OK)
    {
        tb_audio_system_close(system);
        TB_TEST_SKIP("%s has no output: %s", backend, error.message);
    }
    tb_audio_device_start(device);
    const uint64_t deadline = tb_platform_clock_nanoseconds() + WAIT_NANOSECONDS;
    while (tb_platform_atomic_size_load(&calls) < 3 && tb_platform_clock_nanoseconds() < deadline)
    {
        tb_platform_sleep(1000000u);
    }
    tb_audio_device_get_latency(device, &latency);
    TB_TEST_CONTEXT("%s latency: period %u, buffer %u, %.1f ms", backend, latency.period_frames,
                    latency.buffer_frames, latency.seconds * 1000.0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&calls), >=, 3);
    TB_TEST_EXPECT(latency.period_frames > 0);
    tb_audio_device_close(device);

    if (count > 0)
    {
        /* A device the system listed opens by its identifier. */
        desc.device_id = devices[0].id;
        TB_TEST_EXPECT_INT(tb_audio_device_open(system, &desc, &device, &error), ==,
                           TB_AUDIO_DEVICE_OK);
        tb_audio_device_close(device);
    }
    desc.device_id = "no such device, anywhere";
    TB_TEST_EXPECT_INT(tb_audio_device_open(system, &desc, &device, NULL), ==,
                       TB_AUDIO_DEVICE_NOT_FOUND);
    tb_audio_system_close(system);
}

TB_TEST(platform_audio_linux, pipewire_plays)
{
    exercise(tb_pipewire_open, "PipeWire");
}

TB_TEST(platform_audio_linux, alsa_plays)
{
    exercise(tb_alsa_open, "ALSA");
}
