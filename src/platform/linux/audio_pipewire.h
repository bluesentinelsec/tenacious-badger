/* Private to the Linux adapter: the parts of the PipeWire backend that
 * need no PipeWire, which are standard C and tested on every platform: the
 * format a stream offers, as the SPA pod PipeWire reads, and the default
 * sink's name, as PipeWire's "default" metadata holds it. */
#ifndef TB_LINUX_AUDIO_PIPEWIRE_H
#define TB_LINUX_AUDIO_PIPEWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes the largest format pod takes: its header, five properties, and an
 * array of the most channels. */
#define TB_PIPEWIRE_FORMAT_POD_CAPACITY 256u

/* Write the pod of SPA_PARAM_EnumFormat that offers 32-bit floats,
 * interleaved, in the machine's byte order, at `rate`, in `channels`
 * channels laid out as tb_audio_device_channel_mask() says, as
 * spa_format_audio_raw_build() would: into `pod`, which is 8-byte aligned
 * and holds TB_PIPEWIRE_FORMAT_POD_CAPACITY bytes. Returns its bytes. */
size_t tb_pipewire_format_pod(uint32_t rate, uint32_t channels, uint64_t *pod);

/* The SPA channel position of a speaker of WAVE_FORMAT_EXTENSIBLE's mask,
 * one bit; 0, SPA_AUDIO_CHANNEL_UNKNOWN, for any other. */
uint32_t tb_pipewire_position(uint32_t speaker);

/* The node name in a value of the "default" metadata, such as
 * `{"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}`, into `name` of
 * `capacity` bytes. Returns false, with "" in `name`, for a value that has
 * none or one that does not fit. */
bool tb_pipewire_default_name(const char *value, char *name, size_t capacity);

#endif
