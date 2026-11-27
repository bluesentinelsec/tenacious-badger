/* Private to the Win32 adapter: the parts of the WASAPI backend that need
 * no device, for its tests. */
#ifndef TB_WIN32_AUDIO_WASAPI_H
#define TB_WIN32_AUDIO_WASAPI_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mmreg.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The format a stream is opened in: 32-bit floats, interleaved, at `rate`,
 * in `channels` channels laid out as tb_audio_device_channel_mask() says. */
WAVEFORMATEXTENSIBLE tb_wasapi_wave_format(uint32_t rate, uint32_t channels);

/* Whether two of the system's identifiers of a device are the same; NULL
 * is the same only as NULL. */
bool tb_wasapi_same_id(const wchar_t *left, const wchar_t *right);

/* Convert a string of the system's, NUL-terminated UTF-16, to UTF-8 in
 * `text` of `capacity` bytes, cut at a character to fit; "" for NULL or
 * for UTF-16 that is not valid. */
void tb_wasapi_text(const wchar_t *source, char *text, size_t capacity);

/* 100-nanosecond units, as WASAPI times things, as frames at `rate`. */
uint32_t tb_wasapi_frames(LONGLONG duration, uint32_t rate);

#endif
