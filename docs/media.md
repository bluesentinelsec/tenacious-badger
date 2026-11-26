# Media

The `media` module holds what games are made of before the engine plays or
draws them: sounds, and the files they come in. Its code lives in
`src/media/`. Everything is the engine's own C; no media library is used.

## Sounds

`badger/sound.h` is a sound in memory: frames of PCM samples at one rate,
with their format, their channels, and the loops a sampler plays. It is
what a sound effect or a piece of music is once decoded, and what a mixer
will play. Files are read into sounds by their own headers, such as
[`badger/wav.h`](#wav-files); `sound.h` knows no file format.

```c
const tb_sound_spec spec = {.format = TB_SOUND_FORMAT_S16, .sample_rate = 48000, .channels = 2};
tb_sound *beep = NULL;
if (tb_sound_create(&spec, 4800, NULL, &beep) != TB_SOUND_OK) { ... }
tb_sound_write_frames(beep, 0, 4800, tone, 4800 * 2);  /* floats in -1..1 */
tb_sound_fade(beep, 4320, 480, 1.0f, 0.0f);            /* the last 10 ms */
tb_sound_destroy(beep);
```

### Samples

A frame holds one sample of each channel, in the order of the channel
mask's bits, and the frames follow one another: the samples are
interleaved. A sound has 1 to 32 channels, at 1 Hz to 768 kHz.

| Format | Bytes | Stored as | Silence |
| --- | --- | --- | --- |
| `TB_SOUND_FORMAT_U8` | 1 | unsigned, offset by 128 | 128 |
| `TB_SOUND_FORMAT_S16` | 2 | `int16_t`, in the machine's byte order | 0 |
| `TB_SOUND_FORMAT_S24` | 3 | two's complement, least significant byte first | 0 |
| `TB_SOUND_FORMAT_S32` | 4 | `int32_t`, in the machine's byte order | 0 |
| `TB_SOUND_FORMAT_F32` | 4 | `float`, in the machine's byte order | 0.0 |

The channel mask names the speaker of each channel with the bits of
WAVE_FORMAT_EXTENSIBLE (`TB_SOUND_FRONT_LEFT` and its kin). It is 0 when it
is not known, and a player then takes `tb_sound_default_channel_mask`: front
centre for mono, front left and right for stereo, and the documented
layouts up to 7.1.

**Values.** Functions that read or write samples as floats map an integer
sample `s` of `n` bits to `s / 2^(n-1)`, so full scale is -1 and just under
+1. A float is written back as the nearest integer of `f * 2^(n-1)`, halves
away from zero, clamped to the format's range; NaN is written as silence.
F32 samples are not clamped, and may go past ±1. A trip from U8, S16 or
S24 through F32 and back is exact; S32 keeps its top 24 bits. Nothing is
dithered: converting to fewer bits rounds.

**Why formats are kept as they are.** A file's samples are kept in the
format closest to the file's, rather than converted to one format on load.
Writing a sound back then gives the same bytes, and a 16-bit sound takes
half the memory of a float one. A mixer converts what it plays once, when
it loads it, to the format it mixes in, as SDL_mixer converts to the
device's format; `tb_sound_convert_format` is that conversion.

### Making and destroying

| Function | Makes |
| --- | --- |
| `tb_sound_create` | Silent frames of a spec |
| `tb_sound_create_copy` | A copy of the caller's interleaved samples |
| `tb_sound_create_borrowed` | A sound that reads the caller's samples in place |
| `tb_sound_duplicate` | A copy of a sound, with its loops, that owns its samples |
| `tb_sound_copy_range` | A copy of a range of frames, with the loops wholly within it |
| `tb_sound_destroy` | Releases a sound and what it owns |

A spec out of range is `TB_SOUND_INVALID`, and so is a length of samples
that is not a whole number of frames; samples that would need more bytes
than `size_t` counts are `TB_SOUND_TOO_LARGE`. Every sound lives in the
allocator it was made with, or on the heap.

**Borrowed samples are never written.** A borrowed sound reads the caller's
samples until a function would write them; that function copies them into
memory of the sound's own first, and from then on the caller's samples are
not used. Functions that only drop frames from the end, or that make new
samples anyway, such as a change of format, need no copy.
`tb_sound_owns_samples` tells which a sound does.

### Reading

`tb_sound_get_spec`, `tb_sound_frame_count`, `tb_sound_frame_size` and
`tb_sound_duration` describe a sound. `tb_sound_samples` borrows its
samples to read, and `tb_sound_edit_samples` to change in place.
`tb_sound_get_sample` and `tb_sound_set_sample` read and write one sample as
a float; `tb_sound_read_frames` and `tb_sound_write_frames` do it for a
range of frames, interleaved, whatever the sound's format. `tb_sound_peak`
is the largest magnitude of any sample.

### Changing

| Function | Does |
| --- | --- |
| `tb_sound_convert_format` | Stores the samples in another format, by the rules of [values](#samples) |
| `tb_sound_convert_channels` | One to many copies the one at full gain into each; many to one is their mean, a gain of 1/n each, which cannot clip; otherwise the channels both counts have are kept, new ones are silent, and the rest are dropped |
| `tb_sound_resample` | Plays at another rate, keeping pitch and length, by linear interpolation |
| `tb_sound_apply_gain` | Multiplies every sample, clamping the integer formats |
| `tb_sound_fade` | Multiplies a range by a gain that moves in a straight line from its first frame to its last |
| `tb_sound_normalize` | Scales the largest magnitude to a peak, and returns the gain |
| `tb_sound_reverse` | Plays the frames backwards |
| `tb_sound_trim` | Keeps a range of frames |
| `tb_sound_append` | Adds a copy of another sound's frames, of the same format, rate and channels, or of its own |

A fade in is `tb_sound_fade(sound, 0, n, 0.0f, 1.0f)`, and a fade out of
the last `n` frames `tb_sound_fade(sound, frames - n, n, 1.0f, 0.0f)`.

**Resampling.** The new frame count is the old one times the new rate over
the old, rounded up, so no time is lost. Frame `i` is taken at
`i * old / new` frames of the original, between the two frames around it;
the last frame has no neighbour after it and is held. Linear interpolation
is cheap and leaves some aliasing when the rate falls; a filtered resampler
is planned.

Every change goes through one codec of a sample, so each treats every
format alike. That is simple and exact, not fast: a mixer's paths for its
own format are its own.

### Loops

A loop is a range of frames a sampler repeats: forward, ping-pong, or
backward, a number of times or for ever. Music keeps its loop points in a
WAV file's `smpl` chunk, and a sound keeps up to `TB_SOUND_MAX_LOOPS` (16)
of them. `tb_sound_add_loop` refuses an empty loop, one past the last
frame, or a full sound.

Functions that move frames move the loops with them: a range or a trim
keeps the loops wholly within it, moved to its start; resampling scales
them, rounding a start down and an end up; reversing mirrors them;
appending adds the other sound's, moved, as far as there is room.

### Threading

A sound is externally synchronized. Functions that take a `const tb_sound *`
run together on one sound, from any number of threads, while nothing
changes it; a function that changes it runs alone. Different sounds are
independent.

## WAV files

`badger/wav.h` reads WAV files into sounds, whole or a block of frames at a
time, and writes sounds as WAV files.

```c
tb_sound *jump = NULL;
tb_wav_error error = {0};
if (tb_wav_load_file(vfs, "sounds/jump.wav", NULL, &jump, &error) != TB_WAV_OK)
{
    /* error.message: "sounds/jump.wav: fmt  chunk at byte 12: no channels" */
}
```

`tb_wav_read` reads from memory, `tb_wav_read_stream` from a
[stream](streams.md) at its position, and `tb_wav_load_file` from the
[VFS](vfs.md) by name. Each gives a sound that owns its samples.

### What is read

A RIFF file of form `WAVE`, or an RF64 or BW64 file, whose sizes are in its
`ds64` chunk. The `fmt ` chunk is one of:

| Format tag | Bits | Read as |
| --- | --- | --- |
| WAVE_FORMAT_PCM (1) | 1 to 8 | U8 |
| WAVE_FORMAT_PCM | 9 to 16 | S16 |
| WAVE_FORMAT_PCM | 17 to 24 | S24 |
| WAVE_FORMAT_PCM | 25 to 32 | S32 |
| WAVE_FORMAT_IEEE_FLOAT (3) | 32 | F32, bit for bit |
| WAVE_FORMAT_IEEE_FLOAT | 64 | F32, rounded; NaN becomes silence, and what is past a float's range its largest value |
| WAVE_FORMAT_ALAW (6), WAVE_FORMAT_MULAW (7) | 8 | S16, expanded as G.711 says |
| WAVE_FORMAT_IMA_ADPCM (0x11) | 4 | S16, decoded as the IMA's recommendation says |
| WAVE_FORMAT_EXTENSIBLE (0xFFFE) | as above | As its sub-format, with its channel mask |

A sample whose bits do not fill its bytes is read by its bytes, as the
format stores it left-justified: a 12-bit sample in two bytes is S16, and a
24-bit one in four is S32.

The `smpl` chunk gives the sound its loops; a file's loop end is inclusive,
a sound's is not. A loop that does not lie within the frames, or of a type
that is not forward, ping-pong or backward, is dropped, and so are loops past
16. The `fact` chunk bounds the frames of IMA ADPCM, whose last block may be
cut short. Every other chunk is skipped: `LIST`, `cue `, `bext`, `JUNK`, and
any of a name unknown. A chunk of an odd size is followed by a byte of
padding, which the last chunk may lack. A RIFF size of 0, or larger than
the bytes there are, is taken as a writer's placeholder: the chunks run to
the end of the bytes.

Not read: MS ADPCM and other compressed formats inside WAV (MP3, GSM),
which are `TB_WAV_UNSUPPORTED`; big-endian `RIFX` files; the text of
`LIST`/`INFO`; and cue points.

### Untrusted bytes

Every size and offset is checked before it is used: against the bytes
there are, against the RIFF size, and against the limits of a sound. A file
that breaks the format is `TB_WAV_MALFORMED`, with the chunk and the byte at
fault and a short reason; a file past a limit, such as more than 32
channels or a rate above 768 kHz, is `TB_WAV_TOO_LARGE`. Samples that the
allocator cannot hold are `TB_WAV_NO_MEMORY`, which a caller survives, since
a file's size is not the program's choice. A chunk that runs past the end
of the file is damage, `data` included: a file cut short is refused, not
read in part. Tests mutate and cut files of every format thousands of times
under AddressSanitizer.

### Reading a block at a time

Music is long, and plays while it loads. A `tb_wav_reader` reads a file's
header from a stream it borrows, then decodes its frames a block at a
time, into the format a whole read would give:

```c
tb_wav_reader *reader = NULL;
tb_wav_info info = {0};
if (tb_wav_reader_open(&stream, NULL, &reader, &error) != TB_WAV_OK) { ... }
tb_wav_reader_info(reader, &info);  /* spec, frame count, loops */
size_t frames = 0;
while (tb_wav_reader_read(reader, block, sizeof(block), &frames, &error) == TB_WAV_OK &&
       frames != 0)
{
    /* hand `frames` frames to the mixer */
}
tb_wav_reader_seek(reader, info.loops[0].start, &error);  /* back to the loop */
tb_wav_reader_close(reader);
```

A stream that can seek is read as a file: chunks after `data`, where
`smpl` usually is, are found, and `tb_wav_reader_seek` goes to any frame,
in IMA ADPCM by decoding into the block that holds it. A stream that cannot
seek, such as a decompressor, is read in order up to the samples: `fmt `
must come before `data`, and chunks after the samples are not seen. Its
reader cannot seek, and says `TB_WAV_UNSEEKABLE`. `tb_wav_read_stream` is a
reader that reads everything.

### What is written

`tb_wav_write` writes a sound in its own format, at a stream's position,
without seeking: U8, S16, S24 and S32 as WAVE_FORMAT_PCM, and F32 as
WAVE_FORMAT_IEEE_FLOAT with a `fact` chunk of its frames. The format goes
inside WAVE_FORMAT_EXTENSIBLE where the format's documentation requires it:
for more than 2 channels, for integer samples of more than 16 bits, and for
a channel mask other than the default. Loops are written as a `smpl`
chunk. A file read back is the same sound, sample for sample.

RIFF counts in 32 bits, so samples of 4 GiB or more are `TB_WAV_TOO_LARGE`
before anything is written; the writer does not write RF64.

### Threading

Reading and writing are thread-safe for different sounds and streams; each
stream is externally synchronized, as streams are, and so is a reader.
