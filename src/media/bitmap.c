/* Bitmaps: making them, reading and changing their pixels, and moving
 * pixels between them. */
#include "bitmap_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/size.h"

#include <string.h>

/* Alignment of the pixels a bitmap owns: enough for any vector load. */
#define TB_BITMAP_PIXEL_ALIGNMENT ((size_t)16)

/* Luminance weights of Rec. 709 in 1/256ths: 0.2126, 0.7152 and 0.0722,
 * rounded so that they sum to 256 and white stays white. */
#define TB_BITMAP_LUMA_RED 54u
#define TB_BITMAP_LUMA_GREEN 183u
#define TB_BITMAP_LUMA_BLUE 19u
#define TB_BITMAP_LUMA_SHIFT 8u

/* Bits of fraction of bilinear sampling's positions and weights. */
#define TB_BITMAP_SAMPLE_BITS 8u
#define TB_BITMAP_SAMPLE_ONE (1u << TB_BITMAP_SAMPLE_BITS)

/* Bytes of the buffer that rows are swapped through when flipped. */
#define TB_BITMAP_SWAP_CHUNK 256u

/* ---------------------------------------------------------------------------
 * Pixels.
 * ------------------------------------------------------------------------- */

size_t tb_bitmap_format_bytes(tb_bitmap_format format)
{
    TB_ASSERT_MSG(format < TB_BITMAP_FORMAT_COUNT, "a bitmap format");
    return format == TB_BITMAP_GRAY8 ? 1u : 4u;
}

const char *tb_bitmap_format_name(tb_bitmap_format format)
{
    switch (format)
    {
    case TB_BITMAP_RGBA8:
        return "RGBA8";
    case TB_BITMAP_BGRA8:
        return "BGRA8";
    case TB_BITMAP_GRAY8:
        return "GRAY8";
    case TB_BITMAP_FORMAT_COUNT:
        break;
    }
    return "unknown format";
}

uint8_t tb_bitmap_luminance(tb_bitmap_color color)
{
    const uint32_t sum = TB_BITMAP_LUMA_RED * color.r + TB_BITMAP_LUMA_GREEN * color.g +
                         TB_BITMAP_LUMA_BLUE * color.b + (1u << (TB_BITMAP_LUMA_SHIFT - 1u));
    return (uint8_t)(sum >> TB_BITMAP_LUMA_SHIFT);
}

static tb_bitmap_color tb_bitmap_read_color(const uint8_t *pixel, tb_bitmap_format format)
{
    tb_bitmap_color color = {0};

    switch (format)
    {
    case TB_BITMAP_RGBA8:
        color = (tb_bitmap_color){pixel[0], pixel[1], pixel[2], pixel[3]};
        break;
    case TB_BITMAP_BGRA8:
        color = (tb_bitmap_color){pixel[2], pixel[1], pixel[0], pixel[3]};
        break;
    case TB_BITMAP_GRAY8:
    case TB_BITMAP_FORMAT_COUNT:
        color = (tb_bitmap_color){pixel[0], pixel[0], pixel[0], UINT8_MAX};
        break;
    }
    return color;
}

static void tb_bitmap_write_color(uint8_t *pixel, tb_bitmap_format format, tb_bitmap_color color)
{
    switch (format)
    {
    case TB_BITMAP_RGBA8:
        pixel[0] = color.r;
        pixel[1] = color.g;
        pixel[2] = color.b;
        pixel[3] = color.a;
        break;
    case TB_BITMAP_BGRA8:
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
        pixel[3] = color.a;
        break;
    case TB_BITMAP_GRAY8:
    case TB_BITMAP_FORMAT_COUNT:
        pixel[0] = tb_bitmap_luminance(color);
        break;
    }
}

/* A channel times alpha, rounded as the canvas rounds it (image_pixels.c). */
static uint8_t tb_bitmap_multiply(uint32_t channel, uint32_t alpha)
{
    return (uint8_t)((channel * alpha + 127u) / 255u);
}

/* A premultiplied channel divided by its alpha, rounded to nearest. */
static uint8_t tb_bitmap_divide(uint32_t channel, uint32_t alpha)
{
    if (alpha == 0)
    {
        return 0;
    }
    const uint32_t value = (channel * 255u + alpha / 2u) / alpha;
    return (uint8_t)(value > UINT8_MAX ? UINT8_MAX : value);
}

static tb_bitmap_color tb_bitmap_premultiplied_color(tb_bitmap_color color)
{
    return (tb_bitmap_color){tb_bitmap_multiply(color.r, color.a),
                             tb_bitmap_multiply(color.g, color.a),
                             tb_bitmap_multiply(color.b, color.a), color.a};
}

tb_bitmap_color tb_bitmap_unpremultiplied_color(tb_bitmap_color color)
{
    return (tb_bitmap_color){tb_bitmap_divide(color.r, color.a), tb_bitmap_divide(color.g, color.a),
                             tb_bitmap_divide(color.b, color.a), color.a};
}

/* A color as one convention of alpha has it, in another. */
static tb_bitmap_color tb_bitmap_adapt(tb_bitmap_color color, bool from_premultiplied,
                                       bool to_premultiplied)
{
    if (from_premultiplied == to_premultiplied)
    {
        return color;
    }
    return to_premultiplied ? tb_bitmap_premultiplied_color(color)
                            : tb_bitmap_unpremultiplied_color(color);
}

static uint8_t *tb_bitmap_at(const tb_bitmap *bitmap, uint32_t x, uint32_t y)
{
    return bitmap->pixels + (size_t)y * bitmap->stride +
           (size_t)x * tb_bitmap_format_bytes(bitmap->format);
}

/* ---------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

/* Bytes from the first pixel to one past the last, of rows `stride` apart. */
static size_t tb_bitmap_extent(uint32_t width, uint32_t height, size_t stride,
                               tb_bitmap_format format)
{
    if (width == 0 || height == 0)
    {
        return 0;
    }
    size_t rows = 0;
    size_t extent = 0;
    /* coverage-off: dimensions within TB_BITMAP_MAX_DIMENSION overflow only a 32-bit size_t */
    if (!tb_size_multiply(stride, (size_t)height - 1u, &rows) ||
        !tb_size_add(rows, (size_t)width * tb_bitmap_format_bytes(format), &extent))
    {
        tb_fatal("a bitmap of %u by %u pixels is too large to address", width, height);
    }
    /* coverage-on */
    return extent;
}

static void tb_bitmap_check_dimensions(uint32_t width, uint32_t height, tb_bitmap_format format)
{
    TB_ASSERT_MSG(format < TB_BITMAP_FORMAT_COUNT, "a bitmap format");
    TB_ASSERT_MSG(width <= TB_BITMAP_MAX_DIMENSION && height <= TB_BITMAP_MAX_DIMENSION,
                  "a bitmap's dimensions are at most TB_BITMAP_MAX_DIMENSION");
}

static tb_bitmap *tb_bitmap_new(const tb_allocator *allocator, uint32_t width, uint32_t height,
                                tb_bitmap_format format)
{
    tb_bitmap *const bitmap = TB_ALLOCATOR_NEW(allocator, tb_bitmap);
    *bitmap = (tb_bitmap){.allocator = tb_allocator_resolve(allocator),
                          .width = width,
                          .height = height,
                          .format = format};
    return bitmap;
}

/* Bytes of a row of packed pixels, and of all of them. */
static size_t tb_bitmap_packed_size(uint32_t width, uint32_t height, tb_bitmap_format format)
{
    tb_bitmap_check_dimensions(width, height, format);
    const size_t stride = (size_t)width * tb_bitmap_format_bytes(format);
    return tb_bitmap_extent(width, height, stride, format);
}

/* A bitmap that owns packed pixels: zeroed ones, or NULL when it has none. */
static tb_bitmap *tb_bitmap_own(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                                tb_bitmap_format format, const tb_allocator *allocator)
{
    tb_bitmap *const bitmap = tb_bitmap_new(allocator, width, height, format);
    bitmap->pixels = pixels;
    bitmap->stride = (size_t)width * tb_bitmap_format_bytes(format);
    bitmap->size = size;
    bitmap->capacity = size;
    bitmap->owns_pixels = true;
    return bitmap;
}

tb_bitmap *tb_bitmap_try_create(uint32_t width, uint32_t height, tb_bitmap_format format,
                                const tb_allocator *allocator)
{
    const size_t size = tb_bitmap_packed_size(width, height, format);
    uint8_t *pixels = NULL;

    if (size > 0)
    {
        pixels = tb_allocator_try_calloc(allocator, size, 1, TB_BITMAP_PIXEL_ALIGNMENT);
        if (pixels == NULL)
        {
            return NULL;
        }
    }
    return tb_bitmap_own(pixels, size, width, height, format, allocator);
}

tb_bitmap *tb_bitmap_create(uint32_t width, uint32_t height, tb_bitmap_format format,
                            const tb_allocator *allocator)
{
    const size_t size = tb_bitmap_packed_size(width, height, format);
    uint8_t *const pixels =
        size > 0 ? tb_allocator_calloc(allocator, size, 1, TB_BITMAP_PIXEL_ALIGNMENT) : NULL;
    return tb_bitmap_own(pixels, size, width, height, format, allocator);
}

static void tb_bitmap_check_pixels(const void *pixels, size_t size, uint32_t width, uint32_t height,
                                   size_t stride, tb_bitmap_format format)
{
    tb_bitmap_check_dimensions(width, height, format);
    TB_ASSERT_MSG(stride >= (size_t)width * tb_bitmap_format_bytes(format),
                  "a stride holds a row's pixels");
    TB_ASSERT_MSG(size >= tb_bitmap_extent(width, height, stride, format),
                  "the pixels hold every row");
    TB_ASSERT_MSG(pixels != NULL || width == 0 || height == 0, "pixels != NULL");
}

/* Copy rows of pixels of one format between strides. */
static void tb_bitmap_copy_rows(uint8_t *destination, size_t destination_stride,
                                const uint8_t *source, size_t source_stride, size_t row_bytes,
                                uint32_t rows)
{
    for (uint32_t row = 0; row < rows; row++)
    {
        memcpy(destination + (size_t)row * destination_stride, source + (size_t)row * source_stride,
               row_bytes);
    }
}

tb_bitmap *tb_bitmap_create_from(const void *pixels, size_t size, uint32_t width, uint32_t height,
                                 size_t stride, tb_bitmap_format format,
                                 const tb_allocator *allocator)
{
    tb_bitmap_check_pixels(pixels, size, width, height, stride, format);
    tb_bitmap *const bitmap = tb_bitmap_create(width, height, format, allocator);
    if (bitmap->size > 0)
    {
        tb_bitmap_copy_rows(bitmap->pixels, bitmap->stride, pixels, stride, bitmap->stride, height);
    }
    return bitmap;
}

tb_bitmap *tb_bitmap_wrap(void *pixels, size_t size, uint32_t width, uint32_t height, size_t stride,
                          tb_bitmap_format format, const tb_allocator *allocator)
{
    tb_bitmap_check_pixels(pixels, size, width, height, stride, format);
    tb_bitmap *const bitmap = tb_bitmap_new(allocator, width, height, format);
    bitmap->stride = stride;
    bitmap->size = tb_bitmap_extent(width, height, stride, format);
    bitmap->pixels = bitmap->size > 0 ? pixels : NULL;
    return bitmap;
}

bool tb_bitmap_clip(const tb_bitmap *bitmap, tb_bitmap_rect rect, tb_bitmap_rect *clipped)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT(clipped != NULL);
    const int64_t left = rect.x < 0 ? 0 : rect.x;
    const int64_t top = rect.y < 0 ? 0 : rect.y;
    const int64_t wanted_right = (int64_t)rect.x + rect.width;
    const int64_t wanted_bottom = (int64_t)rect.y + rect.height;
    const int64_t right = wanted_right < bitmap->width ? wanted_right : bitmap->width;
    const int64_t bottom = wanted_bottom < bitmap->height ? wanted_bottom : bitmap->height;

    if (right <= left || bottom <= top)
    {
        *clipped = (tb_bitmap_rect){0};
        return false;
    }
    *clipped = (tb_bitmap_rect){(int32_t)left, (int32_t)top, (uint32_t)(right - left),
                                (uint32_t)(bottom - top)};
    return true;
}

tb_bitmap *tb_bitmap_wrap_region(tb_bitmap *parent, tb_bitmap_rect rect,
                                 const tb_allocator *allocator)
{
    tb_bitmap_rect inside = {0};

    TB_ASSERT(parent != NULL);
    (void)tb_bitmap_clip(parent, rect, &inside);
    tb_bitmap *const bitmap = tb_bitmap_new(allocator, inside.width, inside.height, parent->format);
    bitmap->stride = parent->stride;
    bitmap->premultiplied = parent->premultiplied;
    bitmap->size = tb_bitmap_extent(inside.width, inside.height, parent->stride, parent->format);
    if (bitmap->size > 0)
    {
        bitmap->pixels = tb_bitmap_at(parent, (uint32_t)inside.x, (uint32_t)inside.y);
    }
    return bitmap;
}

tb_bitmap *tb_bitmap_copy_rect(const tb_bitmap *bitmap, tb_bitmap_rect rect,
                               const tb_allocator *allocator)
{
    tb_bitmap_rect inside = {0};

    TB_ASSERT(bitmap != NULL);
    (void)tb_bitmap_clip(bitmap, rect, &inside);
    tb_bitmap *const copy =
        tb_bitmap_create(inside.width, inside.height, bitmap->format, allocator);
    copy->premultiplied = bitmap->premultiplied;
    if (copy->size > 0)
    {
        tb_bitmap_copy_rows(copy->pixels, copy->stride,
                            tb_bitmap_at(bitmap, (uint32_t)inside.x, (uint32_t)inside.y),
                            bitmap->stride, copy->stride, copy->height);
    }
    return copy;
}

tb_bitmap *tb_bitmap_duplicate(const tb_bitmap *bitmap, const tb_allocator *allocator)
{
    TB_ASSERT(bitmap != NULL);
    return tb_bitmap_convert(bitmap, bitmap->format, allocator);
}

tb_bitmap *tb_bitmap_convert(const tb_bitmap *bitmap, tb_bitmap_format format,
                             const tb_allocator *allocator)
{
    TB_ASSERT(bitmap != NULL);
    tb_bitmap *const copy = tb_bitmap_create(bitmap->width, bitmap->height, format, allocator);
    copy->premultiplied = bitmap->premultiplied && format != TB_BITMAP_GRAY8;
    if (format == bitmap->format)
    {
        if (copy->size > 0)
        {
            tb_bitmap_copy_rows(copy->pixels, copy->stride, bitmap->pixels, bitmap->stride,
                                copy->stride, copy->height);
        }
        return copy;
    }
    const size_t from_bytes = tb_bitmap_format_bytes(bitmap->format);
    const size_t to_bytes = tb_bitmap_format_bytes(format);
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        const uint8_t *const from = tb_bitmap_const_row(bitmap, y);
        uint8_t *const to = tb_bitmap_row(copy, y);
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            tb_bitmap_write_color(
                to + (size_t)x * to_bytes, format,
                tb_bitmap_read_color(from + (size_t)x * from_bytes, bitmap->format));
        }
    }
    return copy;
}

void tb_bitmap_destroy(tb_bitmap *bitmap)
{
    if (bitmap == NULL)
    {
        return;
    }
    const tb_allocator allocator = bitmap->allocator;
    if (bitmap->owns_pixels && bitmap->capacity > 0)
    {
        tb_allocator_free(&allocator, bitmap->pixels, bitmap->capacity, TB_BITMAP_PIXEL_ALIGNMENT);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_bitmap, bitmap);
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

void tb_bitmap_info_of(const tb_bitmap *bitmap, tb_bitmap_info *info)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT(info != NULL);
    *info = (tb_bitmap_info){.width = bitmap->width,
                             .height = bitmap->height,
                             .format = bitmap->format,
                             .stride = bitmap->stride,
                             .size = bitmap->size,
                             .premultiplied = bitmap->premultiplied,
                             .owns_pixels = bitmap->owns_pixels};
}

uint8_t *tb_bitmap_pixels(tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    return bitmap->pixels;
}

const uint8_t *tb_bitmap_const_pixels(const tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    return bitmap->pixels;
}

uint8_t *tb_bitmap_row(tb_bitmap *bitmap, uint32_t y)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(y < bitmap->height, "a row of the bitmap");
    return bitmap->pixels + (size_t)y * bitmap->stride;
}

const uint8_t *tb_bitmap_const_row(const tb_bitmap *bitmap, uint32_t y)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(y < bitmap->height, "a row of the bitmap");
    return bitmap->pixels + (size_t)y * bitmap->stride;
}

tb_bitmap_color tb_bitmap_get_pixel(const tb_bitmap *bitmap, uint32_t x, uint32_t y)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(x < bitmap->width && y < bitmap->height, "a pixel of the bitmap");
    return tb_bitmap_read_color(tb_bitmap_at(bitmap, x, y), bitmap->format);
}

bool tb_bitmap_equal(const tb_bitmap *left, const tb_bitmap *right)
{
    TB_ASSERT(left != NULL);
    TB_ASSERT(right != NULL);
    if (left->width != right->width || left->height != right->height ||
        left->format != right->format || left->premultiplied != right->premultiplied)
    {
        return false;
    }
    const size_t row_bytes = (size_t)left->width * tb_bitmap_format_bytes(left->format);
    for (uint32_t y = 0; y < left->height && row_bytes > 0; y++)
    {
        if (memcmp(tb_bitmap_const_row(left, y), tb_bitmap_const_row(right, y), row_bytes) != 0)
        {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Changing.
 * ------------------------------------------------------------------------- */

void tb_bitmap_set_pixel(tb_bitmap *bitmap, uint32_t x, uint32_t y, tb_bitmap_color color)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(x < bitmap->width && y < bitmap->height, "a pixel of the bitmap");
    tb_bitmap_write_color(tb_bitmap_at(bitmap, x, y), bitmap->format, color);
}

void tb_bitmap_fill_rect(tb_bitmap *bitmap, tb_bitmap_rect rect, tb_bitmap_color color)
{
    tb_bitmap_rect inside = {0};
    uint8_t pixel[4] = {0};

    TB_ASSERT(bitmap != NULL);
    if (!tb_bitmap_clip(bitmap, rect, &inside))
    {
        return;
    }
    const size_t bytes = tb_bitmap_format_bytes(bitmap->format);
    tb_bitmap_write_color(pixel, bitmap->format, color);
    for (uint32_t y = 0; y < inside.height; y++)
    {
        uint8_t *const row = tb_bitmap_at(bitmap, (uint32_t)inside.x, (uint32_t)inside.y + y);
        if (bytes == 1)
        {
            memset(row, pixel[0], inside.width);
            continue;
        }
        for (uint32_t x = 0; x < inside.width; x++)
        {
            memcpy(row + (size_t)x * bytes, pixel, bytes);
        }
    }
}

void tb_bitmap_fill(tb_bitmap *bitmap, tb_bitmap_color color)
{
    TB_ASSERT(bitmap != NULL);
    tb_bitmap_fill_rect(bitmap, (tb_bitmap_rect){0, 0, bitmap->width, bitmap->height}, color);
}

/* A destination pixel's channel under a premultiplied source's. */
static uint8_t tb_bitmap_over(uint32_t source, uint32_t destination, uint32_t source_alpha)
{
    return (uint8_t)(source + (destination * (255u - source_alpha) + 127u) / 255u);
}

static tb_bitmap_color tb_bitmap_blend_pixel(tb_bitmap_color source, tb_bitmap_color destination)
{
    return (tb_bitmap_color){tb_bitmap_over(source.r, destination.r, source.a),
                             tb_bitmap_over(source.g, destination.g, source.a),
                             tb_bitmap_over(source.b, destination.b, source.a),
                             tb_bitmap_over(source.a, destination.a, source.a)};
}

/* Put one row of `count` pixels of a source on a destination. */
static void tb_bitmap_blit_row(tb_bitmap *destination, uint8_t *to, const tb_bitmap *source,
                               const uint8_t *from, uint32_t count, tb_bitmap_blend blend)
{
    const size_t from_bytes = tb_bitmap_format_bytes(source->format);
    const size_t to_bytes = tb_bitmap_format_bytes(destination->format);

    for (uint32_t x = 0; x < count; x++)
    {
        uint8_t *const out = to + (size_t)x * to_bytes;
        const tb_bitmap_color color =
            tb_bitmap_read_color(from + (size_t)x * from_bytes, source->format);
        if (blend == TB_BITMAP_BLEND_NONE)
        {
            tb_bitmap_write_color(
                out, destination->format,
                tb_bitmap_adapt(color, source->premultiplied, destination->premultiplied));
            continue;
        }
        const tb_bitmap_color over = tb_bitmap_adapt(color, source->premultiplied, true);
        const tb_bitmap_color under = tb_bitmap_adapt(
            tb_bitmap_read_color(out, destination->format), destination->premultiplied, true);
        tb_bitmap_write_color(
            out, destination->format,
            tb_bitmap_adapt(tb_bitmap_blend_pixel(over, under), true, destination->premultiplied));
    }
}

static bool tb_bitmap_rects_overlap(tb_bitmap_rect a, tb_bitmap_rect b)
{
    return (int64_t)a.x < (int64_t)b.x + b.width && (int64_t)b.x < (int64_t)a.x + a.width &&
           (int64_t)a.y < (int64_t)b.y + b.height && (int64_t)b.y < (int64_t)a.y + a.height;
}

void tb_bitmap_blit(tb_bitmap *destination, int32_t x, int32_t y, const tb_bitmap *source,
                    const tb_bitmap_rect *source_rect, tb_bitmap_blend blend)
{
    tb_bitmap_rect from = {0};

    TB_ASSERT(destination != NULL);
    TB_ASSERT(source != NULL);
    TB_ASSERT_MSG(blend == TB_BITMAP_BLEND_NONE || blend == TB_BITMAP_BLEND_ALPHA,
                  "a bitmap blend");
    const tb_bitmap_rect wanted =
        source_rect != NULL ? *source_rect : (tb_bitmap_rect){0, 0, source->width, source->height};
    if (!tb_bitmap_clip(source, wanted, &from))
    {
        return;
    }
    /* Where the clipped source lands, and that clipped to the destination. */
    const int64_t landing_x = (int64_t)x + from.x - wanted.x;
    const int64_t landing_y = (int64_t)y + from.y - wanted.y;
    const int64_t left = landing_x < 0 ? 0 : landing_x;
    const int64_t top = landing_y < 0 ? 0 : landing_y;
    const int64_t right =
        landing_x + from.width < destination->width ? landing_x + from.width : destination->width;
    const int64_t bottom = landing_y + from.height < destination->height ? landing_y + from.height
                                                                         : destination->height;
    if (right <= left || bottom <= top)
    {
        return;
    }
    const tb_bitmap_rect to = {(int32_t)left, (int32_t)top, (uint32_t)(right - left),
                               (uint32_t)(bottom - top)};
    const uint32_t source_x = (uint32_t)(from.x + (left - landing_x));
    const uint32_t source_y = (uint32_t)(from.y + (top - landing_y));
    const tb_bitmap_rect read = {(int32_t)source_x, (int32_t)source_y, to.width, to.height};

    if (source == destination && tb_bitmap_rects_overlap(read, to))
    {
        tb_bitmap *const copy = tb_bitmap_copy_rect(source, read, NULL);
        tb_bitmap_blit(destination, to.x, to.y, copy, NULL, blend);
        tb_bitmap_destroy(copy);
        return;
    }
    const bool same_pixels = source->format == destination->format &&
                             source->premultiplied == destination->premultiplied;
    for (uint32_t row = 0; row < to.height; row++)
    {
        uint8_t *const out = tb_bitmap_at(destination, (uint32_t)to.x, (uint32_t)to.y + row);
        const uint8_t *const in = tb_bitmap_at(source, source_x, source_y + row);
        if (blend == TB_BITMAP_BLEND_NONE && same_pixels)
        {
            memcpy(out, in, (size_t)to.width * tb_bitmap_format_bytes(source->format));
            continue;
        }
        tb_bitmap_blit_row(destination, out, source, in, to.width, blend);
    }
}

/* Swap two runs of bytes that do not overlap. */
static void tb_bitmap_swap(uint8_t *a, uint8_t *b, size_t length)
{
    uint8_t chunk[TB_BITMAP_SWAP_CHUNK] = {0};

    for (size_t done = 0; done < length; done += sizeof(chunk))
    {
        const size_t part = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        memcpy(chunk, a + done, part);
        memcpy(a + done, b + done, part);
        memcpy(b + done, chunk, part);
    }
}

void tb_bitmap_flip_vertical(tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    const size_t row_bytes = (size_t)bitmap->width * tb_bitmap_format_bytes(bitmap->format);
    for (uint32_t top = 0; top < bitmap->height / 2u; top++)
    {
        tb_bitmap_swap(tb_bitmap_row(bitmap, top), tb_bitmap_row(bitmap, bitmap->height - 1u - top),
                       row_bytes);
    }
}

void tb_bitmap_flip_horizontal(tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    const size_t bytes = tb_bitmap_format_bytes(bitmap->format);
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        uint8_t *const row = tb_bitmap_row(bitmap, y);
        for (uint32_t left = 0; left < bitmap->width / 2u; left++)
        {
            tb_bitmap_swap(row + (size_t)left * bytes,
                           row + (size_t)(bitmap->width - 1u - left) * bytes, bytes);
        }
    }
}

/* Apply a change of alpha's convention to every pixel of a bitmap with alpha. */
static void tb_bitmap_readapt(tb_bitmap *bitmap, bool premultiplied)
{
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        uint8_t *const row = tb_bitmap_row(bitmap, y);
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            uint8_t *const pixel = row + (size_t)x * 4u;
            const tb_bitmap_color color = tb_bitmap_read_color(pixel, bitmap->format);
            tb_bitmap_write_color(pixel, bitmap->format,
                                  tb_bitmap_adapt(color, bitmap->premultiplied, premultiplied));
        }
    }
    bitmap->premultiplied = premultiplied;
}

void tb_bitmap_premultiply(tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    if (bitmap->premultiplied || bitmap->format == TB_BITMAP_GRAY8)
    {
        return;
    }
    tb_bitmap_readapt(bitmap, true);
}

void tb_bitmap_unpremultiply(tb_bitmap *bitmap)
{
    TB_ASSERT(bitmap != NULL);
    if (!bitmap->premultiplied)
    {
        return;
    }
    tb_bitmap_readapt(bitmap, false);
}

void tb_bitmap_set_premultiplied(tb_bitmap *bitmap, bool premultiplied)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(!premultiplied || bitmap->format != TB_BITMAP_GRAY8,
                  "a gray bitmap has no alpha to premultiply by");
    bitmap->premultiplied = premultiplied;
}

size_t tb_bitmap_color_key(tb_bitmap *bitmap, tb_bitmap_color key)
{
    static const tb_bitmap_color transparent = {0};
    size_t changed = 0;

    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(bitmap->format != TB_BITMAP_GRAY8, "a color key needs a bitmap with alpha");
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        uint8_t *const row = tb_bitmap_row(bitmap, y);
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            uint8_t *const pixel = row + (size_t)x * 4u;
            const tb_bitmap_color color = tb_bitmap_read_color(pixel, bitmap->format);
            if (color.r == key.r && color.g == key.g && color.b == key.b)
            {
                tb_bitmap_write_color(pixel, bitmap->format, transparent);
                changed++;
            }
        }
    }
    return changed;
}

/* ---------------------------------------------------------------------------
 * Scaling.
 * ------------------------------------------------------------------------- */

/* The source's pixel nearest the middle of a destination's. */
static uint32_t tb_bitmap_nearest(uint32_t index, uint32_t from, uint32_t to)
{
    return (uint32_t)(((uint64_t)index * 2u + 1u) * from / ((uint64_t)to * 2u));
}

/* Where the middle of a destination's pixel falls among the source's
 * middles: the source pixel before it, the one after, and the weight of the
 * one after, in 1/256ths. */
typedef struct tb_bitmap_tap
{
    uint32_t first;
    uint32_t second;
    uint32_t weight;
} tb_bitmap_tap;

static tb_bitmap_tap tb_bitmap_tap_of(uint32_t index, uint32_t from, uint32_t to)
{
    const int64_t position =
        (int64_t)(((uint64_t)index * 2u + 1u) * from * TB_BITMAP_SAMPLE_ONE / ((uint64_t)to * 2u)) -
        (int64_t)(TB_BITMAP_SAMPLE_ONE / 2u);
    if (position <= 0)
    {
        return (tb_bitmap_tap){0, 0, 0};
    }
    const uint32_t first = (uint32_t)(position >> TB_BITMAP_SAMPLE_BITS);
    if (first + 1u >= from)
    {
        return (tb_bitmap_tap){from - 1u, from - 1u, 0};
    }
    return (tb_bitmap_tap){first, first + 1u,
                           (uint32_t)(position & (int64_t)(TB_BITMAP_SAMPLE_ONE - 1u))};
}

static tb_bitmap_color tb_bitmap_premultiplied_at(const tb_bitmap *bitmap, uint32_t x, uint32_t y)
{
    return tb_bitmap_adapt(tb_bitmap_get_pixel(bitmap, x, y), bitmap->premultiplied, true);
}

static uint8_t tb_bitmap_mix(uint8_t top_left, uint8_t top_right, uint8_t bottom_left,
                             uint8_t bottom_right, uint32_t across, uint32_t down)
{
    const uint32_t one = TB_BITMAP_SAMPLE_ONE;
    const uint32_t sum = top_left * (one - across) * (one - down) +
                         top_right * across * (one - down) + bottom_left * (one - across) * down +
                         bottom_right * across * down;
    return (uint8_t)((sum + one * one / 2u) / (one * one));
}

static tb_bitmap_color tb_bitmap_bilinear(const tb_bitmap *bitmap, tb_bitmap_tap across,
                                          tb_bitmap_tap down)
{
    const tb_bitmap_color a = tb_bitmap_premultiplied_at(bitmap, across.first, down.first);
    const tb_bitmap_color b = tb_bitmap_premultiplied_at(bitmap, across.second, down.first);
    const tb_bitmap_color c = tb_bitmap_premultiplied_at(bitmap, across.first, down.second);
    const tb_bitmap_color d = tb_bitmap_premultiplied_at(bitmap, across.second, down.second);
    const tb_bitmap_color mixed = {tb_bitmap_mix(a.r, b.r, c.r, d.r, across.weight, down.weight),
                                   tb_bitmap_mix(a.g, b.g, c.g, d.g, across.weight, down.weight),
                                   tb_bitmap_mix(a.b, b.b, c.b, d.b, across.weight, down.weight),
                                   tb_bitmap_mix(a.a, b.a, c.a, d.a, across.weight, down.weight)};
    return tb_bitmap_adapt(mixed, true, bitmap->premultiplied);
}

tb_bitmap *tb_bitmap_scale(const tb_bitmap *bitmap, uint32_t width, uint32_t height,
                           tb_bitmap_filter filter, const tb_allocator *allocator)
{
    TB_ASSERT(bitmap != NULL);
    TB_ASSERT_MSG(bitmap->width > 0 && bitmap->height > 0, "a bitmap with pixels to scale");
    TB_ASSERT_MSG(width > 0 && height > 0, "a scaled bitmap has pixels");
    TB_ASSERT_MSG(filter == TB_BITMAP_NEAREST || filter == TB_BITMAP_BILINEAR, "a bitmap filter");
    tb_bitmap *const scaled = tb_bitmap_create(width, height, bitmap->format, allocator);
    scaled->premultiplied = bitmap->premultiplied;
    for (uint32_t y = 0; y < height; y++)
    {
        const tb_bitmap_tap down = tb_bitmap_tap_of(y, bitmap->height, height);
        const uint32_t nearest_y = tb_bitmap_nearest(y, bitmap->height, height);
        for (uint32_t x = 0; x < width; x++)
        {
            if (filter == TB_BITMAP_NEAREST)
            {
                const uint32_t nearest_x = tb_bitmap_nearest(x, bitmap->width, width);
                memcpy(tb_bitmap_at(scaled, x, y), tb_bitmap_at(bitmap, nearest_x, nearest_y),
                       tb_bitmap_format_bytes(bitmap->format));
                continue;
            }
            const tb_bitmap_tap across = tb_bitmap_tap_of(x, bitmap->width, width);
            tb_bitmap_write_color(tb_bitmap_at(scaled, x, y), scaled->format,
                                  tb_bitmap_bilinear(bitmap, across, down));
        }
    }
    return scaled;
}
