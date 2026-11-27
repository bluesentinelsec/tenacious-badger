/* A silent hardware check for the macOS default output. The optional wake
 * observation keeps the same stream open while the operator wakes the display. */
#include "../audio_display_state.h"

#include "badger/audio_device.h"
#include "badger/fmt.h"
#include "badger/platform.h"
#include "badger/str.h"

#define OBSERVE_NANOSECONDS 3000000000ull
#define WAKE_TIMEOUT_NANOSECONDS 30000000000ull
#define WAKE_POLL_NANOSECONDS 250000000ull
#define MINIMUM_CALLBACKS 3u

static void count_calls(void *context, float *samples, size_t frames, uint32_t channels)
{
    (void)samples;
    (void)frames;
    (void)channels;
    (void)tb_platform_atomic_size_add(context, 1);
}

static bool observe(tb_platform_atomic_size *calls, bool await_wake)
{
    const tb_test_audio_display initial = tb_test_audio_display_state();
    if (initial == TB_TEST_AUDIO_DISPLAY_UNKNOWN)
    {
        (void)tb_fmt_print_error("Could not determine the main display's power state.\n");
        return false;
    }
    const bool initial_sleep = initial == TB_TEST_AUDIO_DISPLAY_ASLEEP;
    if (await_wake && !initial_sleep)
    {
        (void)tb_fmt_print_error("Wake observation requires an initially sleeping display.\n");
        return false;
    }
    const size_t initial_calls = tb_platform_atomic_size_load(calls);
    tb_platform_sleep(OBSERVE_NANOSECONDS);
    const size_t first = tb_platform_atomic_size_load(calls) - initial_calls;
    const tb_test_audio_display after = tb_test_audio_display_state();
    if (after == TB_TEST_AUDIO_DISPLAY_UNKNOWN)
    {
        (void)tb_fmt_print_error("The main display disappeared during observation.\n");
        return false;
    }
    (void)tb_fmt_print("Display %s at start, %s after 3 s; %zu callbacks on the open output.\n",
                       initial_sleep ? "asleep" : "awake",
                       after == TB_TEST_AUDIO_DISPLAY_ASLEEP ? "asleep" : "awake", first);
    if (!await_wake)
    {
        if (first < MINIMUM_CALLBACKS)
        {
            (void)tb_fmt_print_error("Expected at least three callbacks. Check the default output "
                                     "route and repeat with the display awake.\n");
            return false;
        }
        return true;
    }
    if (after != TB_TEST_AUDIO_DISPLAY_ASLEEP)
    {
        (void)tb_fmt_print_error("Display woke before the initial observation finished.\n");
        return false;
    }
    (void)tb_fmt_print("Wake the Mac now. The stream stays open for up to 30 s.\n");
    (void)tb_fmt_flush();
    const uint64_t deadline = tb_platform_clock_nanoseconds() + WAKE_TIMEOUT_NANOSECONDS;
    while (tb_test_audio_display_state() == TB_TEST_AUDIO_DISPLAY_ASLEEP &&
           tb_platform_clock_nanoseconds() < deadline)
    {
        tb_platform_sleep(WAKE_POLL_NANOSECONDS);
    }
    if (tb_test_audio_display_state() != TB_TEST_AUDIO_DISPLAY_AWAKE)
    {
        (void)tb_fmt_print_error("No display wake observed.\n");
        return false;
    }
    const size_t at_wake = tb_platform_atomic_size_load(calls);
    tb_platform_sleep(OBSERVE_NANOSECONDS);
    const size_t resumed = tb_platform_atomic_size_load(calls) - at_wake;
    (void)tb_fmt_print("Display woke; %zu callbacks on the same output in the next 3 s.\n",
                       resumed);
    return resumed >= MINIMUM_CALLBACKS;
}

int main(int argc, char **argv)
{
    const bool await_wake = argc == 2 && tb_str_equal(argv[1], "--await-wake");
    if (argc != 1 && !await_wake)
    {
        (void)tb_fmt_print_error("Usage: tb_macos_audio_probe [--await-wake]\n");
        return 2;
    }

    tb_audio_system *system = NULL;
    tb_audio_device *device = NULL;
    tb_audio_device_error audio_error = TB_AUDIO_DEVICE_ERROR_INIT;
    tb_platform_atomic_size calls = {0};
    const tb_audio_device_desc desc = {
        .format = {.sample_rate = 48000, .channels = 2}, .render = count_calls, .context = &calls};
    int status = 1;
    if (tb_audio_system_open(&system, &audio_error) != TB_AUDIO_DEVICE_OK ||
        tb_audio_device_open(system, &desc, &device, &audio_error) != TB_AUDIO_DEVICE_OK)
    {
        (void)tb_fmt_print_error("Could not open the default output: %s\n", audio_error.message);
        goto cleanup;
    }
    tb_audio_device_start(device);
    if (observe(&calls, await_wake))
    {
        status = 0;
    }

cleanup:
    tb_audio_device_close(device);
    tb_audio_system_close(system);
    return status;
}
