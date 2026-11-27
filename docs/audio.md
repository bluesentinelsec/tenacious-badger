# Audio devices

An audio system connects to the platform's output service. A game opens one
with `tb_audio_system_open`, lists outputs with `tb_audio_system_list_devices`,
and passes a render callback to `tb_audio_device_open`. A null device ID
selects the default output. To reopen a player's selection, keep the ID from
the device list. The name is for display. Opening can return
`TB_AUDIO_DEVICE_UNAVAILABLE` on a machine with no usable output. The error
message says which service or output failed. The game can continue in silence.

| Platform | Output service | How it feeds the speakers |
| --- | --- | --- |
| Windows | WASAPI | A worker waits on a shared-mode event and fills the available frames. |
| Linux and Steam Deck | PipeWire | The server pulls buffers on the stream's data thread. Its session manager routes playback. |
| Linux without a PipeWire connection | ALSA | A worker renders a period, then writes it to the PCM. When the PCM disappears, it retries the default output. |
| macOS | Core Audio | A HAL output unit pulls samples on its I/O thread. A worker handles changes to the output device. |

The adapters load their system libraries at run time. Linux looks for
`libpipewire-0.3.so.0` and `libasound.so.2`. Query the selected service
with `tb_audio_system_get_info`. Its flags describe default-device
identification, following, and change notifications. ALSA resolves its
default PCM when a stream opens or reconnects. Poll
`tb_audio_device_next_event` for device changes and losses. The queue keeps
the latest 16 events.

## Feeding a stream

The stream opens stopped. Start it with `tb_audio_device_start`. The render
callback receives a zeroed buffer of interleaved 32-bit floats, its frame
count and its channel count. One stereo frame holds the left and right
samples in order. A zero rate or channel request lets the backend choose.
Read the resulting format with `tb_audio_device_get_format`. The callback
keeps that format across device changes.

Use the frame count supplied on each call. A requested period is a hint.
The measured period, buffer size and estimated output delay are available
from `tb_audio_device_get_latency`. Callback sizes can vary when the
hardware rate and the stream rate differ.

The caller serializes changes to a system and its devices. The render
callback runs on the device's thread. Keep its input ready in advance,
perform bounded work, and use atomics or a lock-free queue to exchange data
with the game. Allocation, locks, logging and I/O can stall the device.
`tb_audio_device_stop` waits for any active callback to return. Its context
can then be changed or released before the next start. Close the devices
before closing their system.

See [the public audio-device interface](../include/badger/audio_device.h)
for the complete call contracts and [media](media.md) for PCM sound data.
