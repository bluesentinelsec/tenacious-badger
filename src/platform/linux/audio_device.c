/* Linux audio: PipeWire where its daemon runs, as on the Steam Deck and
 * current desktops, and ALSA where it does not. Each is loaded at run time
 * by its major version, never linked. */
#include "audio_linux.h"

tb_audio_device_result tb_native_audio_open(const tb_audio_system_ops **ops, void **native,
                                            tb_audio_device_error *error)
{
    tb_audio_device_error pipewire_error = TB_AUDIO_DEVICE_ERROR_INIT;
    tb_audio_device_error alsa_error = TB_AUDIO_DEVICE_ERROR_INIT;
    /* Any failure of PipeWire's falls back: a library without its
     * configuration, as in a container, fails to make a context. */
    tb_audio_device_result result = tb_pipewire_open(ops, native, &pipewire_error);

    if (result == TB_AUDIO_DEVICE_OK)
    {
        /* coverage-off: CI's runners run no PipeWire daemon */
        return TB_AUDIO_DEVICE_OK;
        /* coverage-on */
    }
    result = tb_alsa_open(ops, native, &alsa_error);
    if (result != TB_AUDIO_DEVICE_OK)
    {
        return tb_audio_device_fail(error, result, "%s; %s", pipewire_error.message,
                                    alsa_error.message);
    }
    return TB_AUDIO_DEVICE_OK;
}
