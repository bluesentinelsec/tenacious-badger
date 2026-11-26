# Media

The `media` module holds what images, sounds and other media decode to in
memory, and the decoders and encoders of their files. Its code lives in
`src/media/`, and each kind of media has its own public header. Everything
is the engine's own C; no image or audio library is used.

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
RGBA8 bitmap with straight alpha that owns its pixels. BMP is the only
format for now; `TB_BITMAP_UNKNOWN_FORMAT` is any other.

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
