#include "badger/audio_device.h"

#include "not_implemented.h"

/* Standard C has no sound: how a system finds an output device and feeds
 * it is each system's own. */

tb_audio_device_result tb_audio_system_open(tb_audio_system **system, tb_audio_device_error *error)
{
    (void)system;
    (void)error;
    tb_default_not_implemented("platform.audio", "tb_audio_system_open");
}

void tb_audio_system_close(tb_audio_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.audio", "tb_audio_system_close");
}

void tb_audio_system_get_info(const tb_audio_system *system, tb_audio_system_info *info)
{
    (void)system;
    (void)info;
    tb_default_not_implemented("platform.audio", "tb_audio_system_get_info");
}

size_t tb_audio_system_list_devices(tb_audio_system *system, tb_audio_device_info *devices,
                                    size_t capacity)
{
    (void)system;
    (void)devices;
    (void)capacity;
    tb_default_not_implemented("platform.audio", "tb_audio_system_list_devices");
}

tb_audio_device_result tb_audio_device_open(tb_audio_system *system,
                                            const tb_audio_device_desc *desc,
                                            tb_audio_device **device, tb_audio_device_error *error)
{
    (void)system;
    (void)desc;
    (void)device;
    (void)error;
    tb_default_not_implemented("platform.audio", "tb_audio_device_open");
}

void tb_audio_device_close(tb_audio_device *device)
{
    (void)device;
    tb_default_not_implemented("platform.audio", "tb_audio_device_close");
}

void tb_audio_device_start(tb_audio_device *device)
{
    (void)device;
    tb_default_not_implemented("platform.audio", "tb_audio_device_start");
}

void tb_audio_device_stop(tb_audio_device *device)
{
    (void)device;
    tb_default_not_implemented("platform.audio", "tb_audio_device_stop");
}

void tb_audio_device_get_format(const tb_audio_device *device, tb_audio_format *format)
{
    (void)device;
    (void)format;
    tb_default_not_implemented("platform.audio", "tb_audio_device_get_format");
}

void tb_audio_device_get_latency(tb_audio_device *device, tb_audio_device_latency *latency)
{
    (void)device;
    (void)latency;
    tb_default_not_implemented("platform.audio", "tb_audio_device_get_latency");
}

bool tb_audio_device_next_event(tb_audio_device *device, tb_audio_device_event *event)
{
    (void)device;
    (void)event;
    tb_default_not_implemented("platform.audio", "tb_audio_device_next_event");
}
