# Media

The `media` module holds what games are made of before the engine plays or
draws them: bitmaps and sounds in memory, and the files they come in. Its
code lives in `src/media/`, and each kind of media has its own public
header. Everything is the engine's own C; no image, audio or other media
library is used.

## Bitmaps

`badger/bitmap.h` is the CPU's image: a rectangle of pixels in memory that
a tool or a game reads and changes, that an image file decodes to, and that
is handed to the GPU. It is the first part of loading images as SDL_image
does: one call loads a file of any format the engine reads, recognized by
its bytes, as RGBA8.

```c
tb_bitmap *sheet = NULL;
tb_bitmap_error error = {0};
if (tb_bitmap_load(vfs, "sprites/hero.bmp", NULL, NULL, &sheet, &error) != TB_BITMAP_OK)
{
    tb_log_printf(log, TB_LOG_ERROR, "media", "%s", error.message);
    /* sprites/hero.bmp: byte 54: the pixels end early */
    return;
}
tb_bitmap_color_key(sheet, (tb_bitmap_color){255, 0, 255, 255}); /* magenta is transparent */
tb_bitmap *const frame = tb_bitmap_copy_rect(sheet, (tb_bitmap_rect){32, 0, 32, 32}, NULL);
```

A bitmap is opaque. `tb_bitmap_info_of` tells its width, height, format,
stride (the bytes from a row to the next), size, whether its colors are
premultiplied, and whether it owns its pixels. Rows go from the top.

### Formats

| Format | Bytes | For |
| --- | --- | --- |
| `TB_BITMAP_RGBA8` | 4: red, green, blue, alpha | The canonical format: decoders give it, and the canvas takes it |
| `TB_BITMAP_BGRA8` | 4: blue, green, red, alpha | The order of BMP files, Windows bitmaps and swap chains, so their pixels are copied without shuffling |
| `TB_BITMAP_GRAY8` | 1: gray, always opaque | Masks, coverage and height maps, at a quarter of the memory |

`tb_bitmap_convert` copies a bitmap into another format: gray becomes the
same gray in each color channel, opaque, and a color becomes its luminance
(Rec. 709, in integers so that every platform makes the same bytes), with
its alpha dropped. RGB without alpha has no GPU format and saves memory
only on the CPU, and floating-point formats wait for a decoder of HDR
images that needs them; neither is here.

### Making and destroying

| Function | Makes |
| --- | --- |
| `tb_bitmap_create` | A bitmap of zero pixels: transparent black. `tb_bitmap_fill` colors it |
| `tb_bitmap_create_from` | A copy of the caller's pixels, given with their stride |
| `tb_bitmap_wrap` | A bitmap that borrows the caller's pixels: changing it changes them |
| `tb_bitmap_wrap_region` | A bitmap that borrows a rectangle of another's: a sprite sheet's cell, to fill or blit into |
| `tb_bitmap_duplicate` | A copy |
| `tb_bitmap_copy_rect` | A copy of a rectangle, clipped |
| `tb_bitmap_convert` | A copy in another format |
| `tb_bitmap_scale` | A copy at another size, nearest or bilinear |

Each takes the allocator the bitmap lives in, NULL for the heap
([allocators](memory.md#choosing-the-allocator-of-one-object)), and `tb_bitmap_destroy` releases the bitmap
there, and its pixels if it owns them. A bitmap that borrows never releases
what it borrows, which must outlive it. A bitmap that owns its pixels has
rows of exactly its width, with nothing between them; a bitmap of no pixels,
zero wide or zero high, is allowed, and has no pixels to point to. Each side
is at most `TB_BITMAP_MAX_DIMENSION`, 65536 pixels. Running out of memory
ends the process, as allocation does.

### Reading and changing pixels

`tb_bitmap_get_pixel` and `tb_bitmap_set_pixel` read and write one pixel as
a `tb_bitmap_color` of 8 bits a channel, whatever the format stores;
`tb_bitmap_row` and `tb_bitmap_pixels` give the bytes themselves.

| Function | Does |
| --- | --- |
| `tb_bitmap_fill`, `tb_bitmap_fill_rect` | Sets every pixel, or every pixel of a rectangle |
| `tb_bitmap_blit` | Puts a rectangle of one bitmap on another, replacing its pixels or blending over them |
| `tb_bitmap_flip_vertical`, `tb_bitmap_flip_horizontal` | Reverses the rows, or the columns |
| `tb_bitmap_premultiply`, `tb_bitmap_unpremultiply` | Multiplies colors by their alpha, or divides them |
| `tb_bitmap_color_key` | Makes every pixel of one color transparent black |
| `tb_bitmap_equal` | Compares dimensions, format, premultiplication and pixels |

**Rectangles are clipped.** A rectangle may be anywhere: partly or wholly
outside the bitmap, with negative coordinates or sizes that run past the
largest integer. Every function that takes one clips it to the bitmap, and
`tb_bitmap_clip` tells what is left. A blit is clipped to both bitmaps, and
a bitmap may be blitted to itself, overlapping, as if through a copy. A
pixel or a row outside a bitmap is a contract violation.

**Alpha is straight unless the bitmap says otherwise.** `tb_bitmap_premultiply`
multiplies each color by its alpha and marks the bitmap premultiplied,
rounding as the canvas rounds when it premultiplies an image it uploads, so
the bytes are the same either way. `tb_bitmap_set_premultiplied` says what
wrapped pixels are, changing none. Functions that move pixels between
bitmaps keep what they mean: a straight pixel copied to a premultiplied
bitmap is premultiplied on the way, and a premultiplied one written to a
BMP file is made straight. A pixel read or written by itself is as the
bitmap stores it.

**Blending** (`TB_BITMAP_BLEND_ALPHA`) is source over destination,
worked out on premultiplied colors whatever the two bitmaps store:

```
out = source + destination × (1 − source alpha)
```

Half-transparent red over opaque blue is half of each, opaque.
`TB_BITMAP_BLEND_NONE` replaces the destination's pixels, alpha and all.

**Color keys.** A classic sprite sheet marks its background with a color,
often magenta. `tb_bitmap_color_key` makes every pixel of that red, green
and blue transparent black, whatever its alpha, so that a filter that
blends a sprite's edge with its neighbours adds no trace of the key.

**Scaling** samples each new pixel at its middle. `TB_BITMAP_NEAREST` takes
the nearest pixel: pixel art scaled by whole factors stays exact.
`TB_BITMAP_BILINEAR` blends the nearest four by distance and by alpha, so a
transparent pixel's color does not bleed into an opaque neighbour's; it is
for moderate changes of size, and blurs a reduction by more than half,
which a filter of more taps would not.

### Handing a bitmap to the canvas

A bitmap in RGBA8 that owns its pixels is what `tb_image_create` takes
(`badger/canvas.h`): rows from the top, `width * 4` bytes each, with
nothing between them. Its premultiplication goes in the image's
description, so the canvas premultiplies straight pixels as it uploads
them and leaves premultiplied ones alone.

```c
tb_bitmap *bitmap = NULL;
if (tb_bitmap_load(vfs, "sprites/hero.bmp", NULL, NULL, &bitmap, NULL) == TB_BITMAP_OK)
{
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    const tb_image_desc desc = {.width = info.width,
                                .height = info.height,
                                .filter = TB_IMAGE_NEAREST,
                                .premultiplied = info.premultiplied,
                                .name = "hero"};
    tb_image *const hero = tb_image_create(renderer, &desc, tb_bitmap_const_pixels(bitmap), info.size);
    tb_bitmap_destroy(bitmap); /* the image has its own copy */
}
```

A bitmap in another format is converted first (`tb_bitmap_convert`), and a
region of another, whose rows are its parent's stride apart, is copied
(`tb_bitmap_duplicate`), or given a row at a time. `tb_image_update`
replaces part of an image from pixels laid out the same way, such as a
`tb_bitmap_copy_rect` of the part that changed.

### Image files

| Function | Reads |
| --- | --- |
| `tb_bitmap_decode` | A file held in memory |
| `tb_bitmap_read` | A stream, from its position to its end |
| `tb_bitmap_load` | A file of a [VFS](vfs.md), by its name |

Each recognizes the format by the file's bytes, not its name, and gives an
RGBA8 bitmap with straight alpha that owns its pixels: a
[BMP file](#bmp-files) by its `BM`, and a [PNG file](#png-files) by its
signature of eight bytes. It then checks for a [TGA file](#tga-files) by its footer or header. Other data returns `TB_BITMAP_UNKNOWN_FORMAT`.

**Files are untrusted.** Every size, offset, count and dimension is checked
against the bytes there are before it is used, with arithmetic that cannot
overflow, and nothing is read past the end. The options limit what a file
may ask for, before any pixel is allocated:

| Option | Default | Limits |
| --- | --- | --- |
| `max_side` | 16384 | Pixels across, and down |
| `max_pixels` | 64 Mi | Pixels in all: 256 MiB in RGBA8 |
| `max_bytes` | 256 MiB | Bytes `tb_bitmap_read` and `tb_bitmap_load` read |

A file over a limit is `TB_BITMAP_TOO_LARGE`. Memory for the pixels that a
file within the limits asks for is allocated so that it may be refused: a
refusal is `TB_BITMAP_NO_MEMORY`, not the end of the process.

**Errors say where and why**, as the JSON reader's do. A `tb_bitmap_error`
holds the result, the byte of the file at fault, a reason, and a message for
a log that puts them together with the file's name, which `tb_bitmap_load`
knows and the options' `name` gives the others:

```
sprites/hero.bmp: byte 26: planes other than 1
```

A fault in a PNG file's chunk names the chunk too, in the error's `chunk`:

```
sprites/hero.png: IHDR chunk at byte 24: a bit depth the color type does not have
```

| Result | Means |
| --- | --- |
| `TB_BITMAP_MALFORMED` | The file is damaged: a field no file has, or bytes that end early |
| `TB_BITMAP_UNSUPPORTED` | A form of the format the engine does not read, such as a BMP file holding a PNG |
| `TB_BITMAP_UNKNOWN_FORMAT` | Bytes of no format the engine reads |
| `TB_BITMAP_TOO_LARGE` | Over the options' limits |
| `TB_BITMAP_NOT_FOUND`, `TB_BITMAP_INPUT_FAILED` | The VFS has no such file, or the file or stream could not be read |

### BMP files

BMP is Windows' device-independent bitmap, read in every form that tools
still write:

| Part | Read |
| --- | --- |
| Headers | `BITMAPCOREHEADER` (OS/2 1.x), `BITMAPINFOHEADER`, and its versions 2, 3, 4 (`BITMAPV4HEADER`) and 5 (`BITMAPV5HEADER`) |
| Rows | From the bottom, as is usual, or from the top, for a negative height; the last row may lack its padding |
| 1, 4 and 8 bits | A palette of up to 256 colors, of three bytes each after a core header and four after the others |
| 16 bits | 5-5-5 without masks; any masks with `BI_BITFIELDS` or `BI_ALPHABITFIELDS`, such as 5-6-5 or 4-4-4-4 |
| 24 bits | Blue, green, red |
| 32 bits | Without masks, the fourth byte is alpha, unless it is zero in every pixel, when the image is opaque. With masks, as they say, alpha too |
| Compression | `BI_RGB`, `BI_BITFIELDS`, `BI_ALPHABITFIELDS`, `BI_RLE8` and `BI_RLE4` |

- **Masks** are contiguous bits within the pixel's. Each channel is widened
  to 8 bits with rounding, so a channel at full is 255 whatever its width.
  A channel without a mask is 0, and alpha without one is opaque.
- **Palettes** hold as many colors as the header counts, or as the bits
  address when it counts none, but no more than fit before the pixels.
  The palette's fourth byte is reserved, and palette colors are opaque. A
  pixel whose index is past the palette makes the file malformed.
- **Run-length encoding** goes from the bottom row, as the format requires.
  Pixels that no run reaches, past the end of a line or skipped by a move,
  are transparent black. A file may end between commands without marking
  its end; a run or a move past the image's edge makes it malformed.
- **Not read:** BMP files holding a JPEG or PNG image, CMYK pixels, OS/2
  2.x headers, and the OS/2 formats of other signatures (`BA`, `CI`, `CP`,
  `IC`, `PT`), all `TB_BITMAP_UNSUPPORTED` or unknown; color profiles and
  gamma, which are ignored, the pixels being taken as sRGB.

`tb_bitmap_write_bmp` writes a bitmap to a stream, rows from the bottom:

| Depth | Writes | Reads back |
| --- | --- | --- |
| `TB_BITMAP_BMP_32` | 32 bits with a version 5 header whose masks say where alpha is, marked sRGB | Every pixel exactly |
| `TB_BITMAP_BMP_24` | 24 bits with the plain header that every reader knows | The colors; alpha is dropped |

A premultiplied bitmap is written straight, as BMP files are, and a gray
one as its gray in each color channel. A bitmap of no pixels is
`TB_BITMAP_INVALID`: no BMP file holds one.

### PNG files

PNG is the format most sprite sheets, textures and UI art come in, and is
read in every form the specification allows:

| Color type | Bit depths | Read as |
| --- | --- | --- |
| Gray (0) | 1, 2, 4, 8, 16 | Its gray in each color channel; opaque, or transparent where `tRNS` names the gray |
| RGB (2) | 8, 16 | Its colors; opaque, or transparent where `tRNS` names the color |
| Palette (3) | 1, 2, 4, 8 | The `PLTE` color of each index, with the alpha `tRNS` gives the first entries |
| Gray and alpha (4) | 8, 16 | Its gray, and its alpha |
| RGBA (6) | 8, 16 | As it is |

- **Samples of fewer than 8 bits** are spread evenly over 0 to 255: a 1-bit
  sample is 0 or 255, a 2-bit one a multiple of 85, a 4-bit one of 17.
  **16 bits** are rounded to the nearest 8 (`v * 255 / 65535`), after
  `tRNS` has compared the full 16, as the specification requires.
- **Filters and interlacing.** Each row's filter (none, sub, up, average or
  Paeth) is undone, and an Adam7 image's seven passes are put where their
  pixels go; a pass of no pixels, in an image narrower or shorter than
  eight, has no rows.
- **Chunks.** `IHDR` comes first, `PLTE` and `tRNS` before the image data,
  the `IDAT` chunks one after another, and `IEND` last; anything after
  `IEND` is ignored. The image data is one zlib stream across its `IDAT`
  chunks, which may be of any size, empty ones included. Every chunk's CRC
  is checked. A critical chunk the engine does not know, its name beginning
  with a capital, is `TB_BITMAP_UNSUPPORTED`; every ancillary one is
  skipped.
- **Gamma and color spaces** (`gAMA`, `sRGB`, `iCCP`, `cHRM`) are
  recognized as ancillary and not applied: the pixels are taken as sRGB, as
  the BMP reader takes them, as games and their tools expect, and as
  SDL_image and stb_image read them. A file of another gamma decodes to
  the values it stores. Converting between sRGB and linear light is for
  filtering and the GPU, not for loading ([#361](https://github.com/bluesentinelsec/tenacious-badger/issues/361)).
- **Animated PNG.** An APNG file's frames are in ancillary chunks (`acTL`,
  `fcTL`, `fdAT`), and it is read as the image its `IDAT` chunks hold: the
  first frame, or, when that image is not part of the animation, the image
  shown by readers that do not animate.
- **Damage.** A palette that is empty, longer than 256 colors or than the
  bit depth addresses, or in a gray image; a `tRNS` of the wrong length, or
  of more alphas than colors; a pixel whose index is past the palette; a
  filter type that is not one of the five; and image data that is not
  zlib, that holds fewer or more rows than the image, or whose Adler-32 is
  wrong, are each `TB_BITMAP_MALFORMED`, with the chunk at fault. Data after
  the zlib stream's end, which some writers leave, is ignored, and so is a
  `tRNS` chunk in an image with alpha, which the specification forbids and
  readers ignore.

**Memory.** The image data is decompressed a row at a time into two rows,
the row and the one above, which are unfiltered and put in the bitmap: a
decoder holds the bitmap, two rows and an inflater's window of 32 KiB and its
code tables, however large
the file. Before the bitmap is allocated the dimensions are checked against
the limits, and against the image data there is: DEFLATE makes at most
1032 bytes of one, so image data too short for the image's rows is damage,
found before the bitmap is allocated. A file of a few bytes cannot make the
decoder allocate 256 MiB.

`tb_bitmap_write_png` writes a bitmap as a PNG file of 8 bits a sample, not
interlaced, marked sRGB:

| `color` of `tb_bitmap_png_options` | Writes |
| --- | --- |
| `TB_BITMAP_PNG_AUTO`, the default | The fewest samples that keep every pixel: gray when every pixel is opaque and gray, RGB when every one is opaque, RGBA otherwise |
| `TB_BITMAP_PNG_RGBA8` | Red, green, blue and alpha |
| `TB_BITMAP_PNG_RGB8` | Red, green and blue; alpha is dropped |
| `TB_BITMAP_PNG_GRAY8` | Luminance, as `tb_bitmap_convert` makes it; alpha is dropped |

Each row is filtered by each of the five filters, and written with the one
whose bytes, taken as signed, sum to the least magnitude: the heuristic of
the PNG specification and of libpng, which suits images of color. The rows
are compressed by the engine's DEFLATE at the options' `level`, 1 to 9, 6
by default, into `IDAT` chunks of 64 KiB. Files read back exactly in
every color type that keeps the pixels.

### TGA files

The bitmap loaders recognize a TGA v2 footer or a plausible 18-byte header
after checking PNG and BMP signatures. They accept raw and run-length
encoded images, all four origins, gray and gray-alpha pixels, 15/16-bit
packed color, 24/32-bit BGR(A), and 8/16-bit indices into a color map.

The extension area's attributes type takes precedence over the descriptor's
alpha bits. A premultiplied file decodes to straight RGBA. For 32-bit color
with unspecified alpha, the reader keeps the alpha channel if any pixel has
nonzero alpha and makes the image opaque otherwise.

`tb_bitmap_write_tga` writes straight 32-bit BGRA from the top left,
using raw pixels or row-by-row run-length encoding. Both modes write the
version 2 footer and preserve alpha.

### Threading

A bitmap is externally synchronized: any number of threads may read one
while none changes it. Making a new bitmap from others, and decoding, are
thread-safe for as long as the bitmaps read are not changed, and as the
allocator given is.

### Testing

`tests/media/bitmap_test.c` covers every operation, in every format, and
every edge of clipping, including rectangles that run past the largest
integer. `tests/media/bitmap_bmp_test.c` builds BMP files byte by byte for
each header, depth and compression; round-trips the writer's files exactly;
refuses each way a header, a palette, a mask or a run can be damaged, at
the byte at fault; and decodes every cut of valid files, and thousands of
copies with random bytes changed, which the sanitizer build proves never
read past the end, crash or leak.

`tests/media/bitmap_png_test.c` reads, in the manner of PngSuite, every
color type at every bit depth, interlaced and not, with and without
transparency, each row of a different filter, from files that encoders
other than the engine's wrote: Python's zlib with filters of its own, and
Windows' WIC and GDI+ (`tools/make_png_test_data.py`, which writes them to
`tests/data/png/`). Every file holds one pattern, which the test computes,
so each pixel is checked. It round-trips the writer's files exactly, in
every color type and at every level. `tests/media/bitmap_png_malformed_test.c`
refuses each way a chunk, the header, the palette, transparency and the
image data can be damaged, with the chunk and byte at fault; refuses a
decompression bomb without allocating; and decodes every cut of files of
each color type, and thousands of copies with random bytes changed and
their CRCs made right again, so that the damage reaches past the CRC
check, under the sanitizers.

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
