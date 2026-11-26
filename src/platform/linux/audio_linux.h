/* Private to the Linux adapter: its two audio backends, which
 * tb_native_audio_open() tries in turn, and which the tests open each by
 * itself. */
#ifndef TB_LINUX_AUDIO_LINUX_H
#define TB_LINUX_AUDIO_LINUX_H

#include "../shared/audio_device.h"

/* Connect to the PipeWire daemon of the session, through
 * libpipewire-0.3.so.0. UNAVAILABLE when the library is missing or no
 * daemon runs. */
tb_audio_device_result tb_pipewire_open(const tb_audio_system_ops **ops, void **native,
                                        tb_audio_device_error *error);

/* Load ALSA, libasound.so.2. UNAVAILABLE when the library is missing. */
tb_audio_device_result tb_alsa_open(const tb_audio_system_ops **ops, void **native,
                                    tb_audio_device_error *error);

#endif
