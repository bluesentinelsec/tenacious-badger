/* What the audio device interface does the same on every platform, and so
 * needs nothing of one: the names of its results. */
#include "badger/audio_device.h"

const char *tb_audio_device_result_name(tb_audio_device_result result)
{
    switch (result)
    {
    case TB_AUDIO_DEVICE_OK:
        return "ok";
    case TB_AUDIO_DEVICE_UNAVAILABLE:
        return "unavailable";
    case TB_AUDIO_DEVICE_NOT_FOUND:
        return "not found";
    case TB_AUDIO_DEVICE_SYSTEM_ERROR:
        return "system error";
    }
    return "unknown result";
}
