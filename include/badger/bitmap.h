/** @file bitmap.h
 * @brief Bitmaps: pixels in memory that the CPU reads and changes, and the
 * image files they are read from and written to.
 *
 * A bitmap is a rectangle of pixels in one format, rows from the top, each
 * row `stride` bytes after the one before. It is what an image file decodes
 * to, what a tool or a game draws into on the CPU, and what is handed to the
 * GPU: an RGBA8 bitmap's pixels are what tb_image_create() takes
 * (badger/canvas.h). It is the first part of loading images as SDL_image
 * does: files are recognized by their bytes, and decode to RGBA8.
 *
 * @code
 * tb_bitmap_error error = {0};
 * tb_bitmap *sheet = NULL;
 * if (tb_bitmap_load(vfs, "sprites/hero.bmp", NULL, NULL, &sheet, &error) != TB_BITMAP_OK)
 * {
 *     tb_log_printf(log, TB_LOG_ERROR, "media", "%s", error.message);
 *     // sprites/hero.bmp: byte 54: the pixels end early
 *     return;
 * }
 * tb_bitmap_color_key(sheet, (tb_bitmap_color){255, 0, 255, 255}); // magenta is transparent
 * tb_bitmap *const frame = tb_bitmap_copy_rect(sheet, (tb_bitmap_rect){32, 0, 32, 32}, NULL);
 * @endcode
 *
 * **Formats.** RGBA8 is the canonical format: decoders give it, and the
 * canvas takes it. BGRA8 is the order of BMP files, Windows bitmaps and
 * swap chains. GRAY8 is one channel: masks, coverage and height maps.
 *
 * **Ownership.** A bitmap owns its pixels, or borrows the caller's
 * (tb_bitmap_wrap()) or another bitmap's (tb_bitmap_wrap_region()). Either
 * way the bitmap itself is allocated from the allocator it was made with,
 * and tb_bitmap_destroy() releases it there; a bitmap that borrows never
 * releases the pixels it borrows, which must outlive it. A bitmap that owns
 * its pixels has rows of exactly `width` pixels, with nothing between them.
 *
 * **Alpha** is straight unless a bitmap says it is premultiplied
 * (tb_bitmap_premultiply()). Functions that move pixels between bitmaps,
 * copying, blending, converting or scaling, keep what pixels mean: a pixel
 * of a straight bitmap blended over a premultiplied one is premultiplied on
 * the way. A pixel read or written by itself is as the bitmap stores it.
 *
 * **Files are untrusted.** Decoding checks every size, offset and count
 * against the bytes there are, refuses dimensions over the limits of its
 * options, never reads past the end, and reports where a file is wrong and
 * why, as the JSON reader does.
 *
 * **Contract violations**, such as a pixel outside a bitmap or a
 * dimension over #TB_BITMAP_MAX_DIMENSION, fail an assertion; running out of
 * memory ends the process, as allocation does, except where decoding says
 * otherwise.
 *
 * @par Threading
 * A bitmap is externally synchronized: any number of threads may read one
 * through a const pointer while none changes it. Functions that make a new
 * bitmap from others, and the decoders, are thread-safe for as long as the
 * bitmaps they read are not changed, and as the allocator given is.
 * tb_bitmap_format_bytes(), tb_bitmap_format_name() and
 * tb_bitmap_result_name() are thread-safe.
 */
#ifndef TB_BITMAP_H
#define TB_BITMAP_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_vfs;

/** @brief The most pixels across or down any bitmap has. */
#define TB_BITMAP_MAX_DIMENSION ((uint32_t)65536)

/** @brief What one pixel holds, and in what order. */
typedef enum tb_bitmap_format
{
    TB_BITMAP_RGBA8, /**< Red, green, blue, alpha: a byte each. The canonical format. */
    TB_BITMAP_BGRA8, /**< Blue, green, red, alpha: a byte each, as BMP files and Windows have it. */
    TB_BITMAP_GRAY8, /**< One byte of gray. Its alpha is always 255. */
    TB_BITMAP_FORMAT_COUNT /**< The number of formats. */
} tb_bitmap_format;

/** @brief A color of 8 bits a channel, whatever the format stores. */
typedef struct tb_bitmap_color
{
    uint8_t r; /**< Red. */
    uint8_t g; /**< Green. */
    uint8_t b; /**< Blue. */
    uint8_t a; /**< Alpha: 0 transparent, 255 opaque. */
} tb_bitmap_color;

/** @brief A rectangle of pixels: its top left, which may be outside a
 * bitmap, and its size. Functions that take one clip it to the bitmap. */
typedef struct tb_bitmap_rect
{
    int32_t x;       /**< Left column. */
    int32_t y;       /**< Top row. */
    uint32_t width;  /**< Columns. */
    uint32_t height; /**< Rows. */
} tb_bitmap_rect;

/** @brief How a pixel of a source is put on its destination. */
typedef enum tb_bitmap_blend
{
    TB_BITMAP_BLEND_NONE, /**< Replace the destination's pixels, alpha and all. */
    /** Source over destination (Porter and Duff): the source's alpha says how
     * much of it covers the destination. Worked out on premultiplied colors,
     * whatever the two bitmaps store. */
    TB_BITMAP_BLEND_ALPHA
} tb_bitmap_blend;

/** @brief How tb_bitmap_scale() finds a pixel between the source's. */
typedef enum tb_bitmap_filter
{
    TB_BITMAP_NEAREST, /**< The nearest: pixel art stays sharp. */
    /** The nearest four, weighted by distance and by alpha, so that the
     * colors of transparent pixels do not bleed into their neighbours. */
    TB_BITMAP_BILINEAR
} tb_bitmap_filter;

/** @brief What a bitmap is. */
typedef struct tb_bitmap_info
{
    uint32_t width;          /**< Pixels across. */
    uint32_t height;         /**< Pixels down. */
    tb_bitmap_format format; /**< What a pixel holds. */
    size_t stride;           /**< Bytes from a row to the next. */
    /** Bytes from the first pixel to one past the last: `stride * (height - 1)`
     * plus a row's pixels; 0 for a bitmap of no pixels. */
    size_t size;
    bool premultiplied; /**< Colors are premultiplied by their alpha. */
    bool owns_pixels;   /**< tb_bitmap_destroy() releases the pixels too. */
} tb_bitmap_info;

/** @brief A bitmap. Opaque; made by the functions below, and destroyed by
 * tb_bitmap_destroy(). */
typedef struct tb_bitmap tb_bitmap;

/* -------------------------------------------------------------------------
 * Formats.
 * ------------------------------------------------------------------------- */

/** @brief Bytes of one pixel of a format: 4, or 1 for gray.
 * @param format One of #tb_bitmap_format, below the count. */
TB_API size_t tb_bitmap_format_bytes(tb_bitmap_format format);

/** @brief A short name of a format, such as "RGBA8"; "unknown format" for a
 * value that is none. */
TB_API const char *tb_bitmap_format_name(tb_bitmap_format format);

/* -------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make a bitmap whose pixels are zero: transparent black, or black
 * for gray. Fill it with tb_bitmap_fill() for another color.
 * @param width Pixels across; at most #TB_BITMAP_MAX_DIMENSION. Zero makes a
 * bitmap of no pixels, as does a zero height.
 * @param height Pixels down; likewise.
 * @param format What a pixel holds.
 * @param allocator Where the bitmap and its pixels live, or NULL for the heap.
 * @return The bitmap, which owns its pixels; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_create(uint32_t width, uint32_t height, tb_bitmap_format format,
                                   const tb_allocator *allocator);

/**
 * @brief Make a bitmap from a copy of the caller's pixels.
 * @param pixels Rows from the top, `stride` bytes apart; NULL only for a
 * bitmap of no pixels. Not kept.
 * @param size Bytes of `pixels`: at least what #tb_bitmap_info says of
 * `size` for these dimensions.
 * @param width Pixels across. @param height Pixels down.
 * @param stride Bytes from a row to the next: at least a row's pixels.
 * @param format What a pixel holds.
 * @param allocator Where the bitmap lives, or NULL for the heap.
 * @return The bitmap, which owns its copy, its rows packed; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_create_from(const void *pixels, size_t size, uint32_t width,
                                        uint32_t height, size_t stride, tb_bitmap_format format,
                                        const tb_allocator *allocator);

/**
 * @brief Make a bitmap that borrows the caller's pixels: changing the bitmap
 * changes them.
 * @param pixels As tb_bitmap_create_from(); they must outlive the bitmap.
 * @param size As tb_bitmap_create_from(). @param width As tb_bitmap_create_from().
 * @param height As tb_bitmap_create_from(). @param stride As tb_bitmap_create_from().
 * @param format As tb_bitmap_create_from().
 * @param allocator Where the bitmap itself lives, or NULL for the heap.
 * @return The bitmap, straight until tb_bitmap_set_premultiplied() says
 * otherwise; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_wrap(void *pixels, size_t size, uint32_t width, uint32_t height,
                                 size_t stride, tb_bitmap_format format,
                                 const tb_allocator *allocator);

/**
 * @brief Make a bitmap of a rectangle of another, borrowing its pixels: a
 * view through which a sprite sheet's cell, say, is filled or blitted to.
 * @param parent Required; it must outlive the new bitmap.
 * @param rect Clipped to the parent; a rectangle outside it makes a bitmap
 * of no pixels.
 * @param allocator Where the bitmap itself lives, or NULL for the heap.
 * @return The bitmap, with the parent's format, stride and premultiplication;
 * never NULL.
 */
TB_API tb_bitmap *tb_bitmap_wrap_region(tb_bitmap *parent, tb_bitmap_rect rect,
                                        const tb_allocator *allocator);

/**
 * @brief Make a copy of a bitmap that owns its pixels, its rows packed.
 * @param bitmap Required. @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_duplicate(const tb_bitmap *bitmap, const tb_allocator *allocator);

/**
 * @brief Make a bitmap of a copy of a rectangle of another.
 * @param bitmap Required.
 * @param rect Clipped to the bitmap; outside it, the copy has no pixels.
 * @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_copy_rect(const tb_bitmap *bitmap, tb_bitmap_rect rect,
                                      const tb_allocator *allocator);

/**
 * @brief Make a copy of a bitmap in another format. Gray becomes the same
 * gray in each color channel, opaque; a color becomes its luminance
 * (Rec. 709), and its alpha is dropped.
 * @param bitmap Required. @param format The copy's format.
 * @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy, premultiplied as the bitmap is, or straight in gray; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_convert(const tb_bitmap *bitmap, tb_bitmap_format format,
                                    const tb_allocator *allocator);

/**
 * @brief Make a copy of a bitmap at another size. Each of its pixels is
 * sampled at its middle.
 * @param bitmap Required; it has pixels.
 * @param width Pixels across; more than 0, at most #TB_BITMAP_MAX_DIMENSION.
 * @param height Pixels down; likewise.
 * @param filter How a pixel is found between the source's.
 * @param allocator Where the copy lives, or NULL for the heap.
 * @return The copy, in the bitmap's format and premultiplication; never NULL.
 */
TB_API tb_bitmap *tb_bitmap_scale(const tb_bitmap *bitmap, uint32_t width, uint32_t height,
                                  tb_bitmap_filter filter, const tb_allocator *allocator);

/** @brief Release a bitmap, and its pixels if it owns them, to the
 * allocator it was made with. @param bitmap A bitmap, or NULL to do nothing. */
TB_API void tb_bitmap_destroy(tb_bitmap *bitmap);

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/** @brief What a bitmap is. @param bitmap Required. @param[out] info Required. */
TB_API void tb_bitmap_info_of(const tb_bitmap *bitmap, tb_bitmap_info *info);

/** @brief The first byte of the top row, to change; NULL for a bitmap of no
 * pixels. @param bitmap Required. */
TB_API uint8_t *tb_bitmap_pixels(tb_bitmap *bitmap);

/** @brief The first byte of the top row, to read; NULL for a bitmap of no
 * pixels. @param bitmap Required. */
TB_API const uint8_t *tb_bitmap_const_pixels(const tb_bitmap *bitmap);

/** @brief The first byte of a row, to change.
 * @param bitmap Required. @param y Below the height. */
TB_API uint8_t *tb_bitmap_row(tb_bitmap *bitmap, uint32_t y);

/** @brief The first byte of a row, to read.
 * @param bitmap Required. @param y Below the height. */
TB_API const uint8_t *tb_bitmap_const_row(const tb_bitmap *bitmap, uint32_t y);

/**
 * @brief A pixel, as the bitmap stores it: premultiplied if the bitmap is.
 * @param bitmap Required. @param x Below the width. @param y Below the height.
 * @return Its color; a gray pixel is its gray in each color channel, opaque.
 */
TB_API tb_bitmap_color tb_bitmap_get_pixel(const tb_bitmap *bitmap, uint32_t x, uint32_t y);

/**
 * @brief Clip a rectangle to a bitmap.
 * @param bitmap Required. @param rect Anywhere.
 * @param[out] clipped Required; the part of `rect` inside the bitmap, of
 * no size when there is none.
 * @return Whether any of `rect` is inside the bitmap.
 */
TB_API bool tb_bitmap_clip(const tb_bitmap *bitmap, tb_bitmap_rect rect, tb_bitmap_rect *clipped);

/**
 * @brief Whether two bitmaps have the same dimensions, format,
 * premultiplication and pixels. Their strides may differ.
 * @param left Required. @param right Required.
 */
TB_API bool tb_bitmap_equal(const tb_bitmap *left, const tb_bitmap *right);

/* -------------------------------------------------------------------------
 * Changing.
 * ------------------------------------------------------------------------- */

/**
 * @brief Set a pixel, as the bitmap stores it. A gray bitmap stores the
 * color's luminance, and drops its alpha.
 * @param bitmap Required. @param x Below the width. @param y Below the height.
 * @param color The color.
 */
TB_API void tb_bitmap_set_pixel(tb_bitmap *bitmap, uint32_t x, uint32_t y, tb_bitmap_color color);

/** @brief Set every pixel to a color, as tb_bitmap_set_pixel() does.
 * @param bitmap Required. @param color The color. */
TB_API void tb_bitmap_fill(tb_bitmap *bitmap, tb_bitmap_color color);

/** @brief Set every pixel of a rectangle, clipped to the bitmap.
 * @param bitmap Required. @param rect Anywhere. @param color The color. */
TB_API void tb_bitmap_fill_rect(tb_bitmap *bitmap, tb_bitmap_rect rect, tb_bitmap_color color);

/**
 * @brief Put a rectangle of one bitmap on another, with its top left at
 * (`x`, `y`), clipped to both. Formats are converted as tb_bitmap_convert()
 * converts them, and alpha as the two bitmaps' premultiplication says.
 *
 * A bitmap may be blitted to itself, and the rectangles may overlap. A
 * bitmap that borrows another's pixels is a different bitmap, and must not
 * be blitted to it where their pixels overlap.
 * @param destination Required. @param x Left column, anywhere. @param y Top row, anywhere.
 * @param source Required.
 * @param source_rect The source's rectangle, clipped to it; NULL for all of it.
 * @param blend How the source's pixels are put on the destination's.
 */
TB_API void tb_bitmap_blit(tb_bitmap *destination, int32_t x, int32_t y, const tb_bitmap *source,
                           const tb_bitmap_rect *source_rect, tb_bitmap_blend blend);

/** @brief Reverse the order of the rows: the top row becomes the bottom.
 * @param bitmap Required. */
TB_API void tb_bitmap_flip_vertical(tb_bitmap *bitmap);

/** @brief Reverse the order of the columns: the left becomes the right.
 * @param bitmap Required. */
TB_API void tb_bitmap_flip_horizontal(tb_bitmap *bitmap);

/**
 * @brief Multiply each color by its alpha, and mark the bitmap
 * premultiplied: what blending and filtering on the GPU want. Rounds as the
 * canvas does when it premultiplies an image it uploads, so the pixels are
 * the same either way. A bitmap already premultiplied, and a gray one, are
 * left as they are.
 * @param bitmap Required.
 */
TB_API void tb_bitmap_premultiply(tb_bitmap *bitmap);

/**
 * @brief Divide each color by its alpha, and mark the bitmap straight. A
 * pixel of no alpha becomes transparent black. Premultiplying lost what a
 * color was below 8 bits of its alpha, so a round trip changes pixels that
 * are neither transparent nor opaque. A straight bitmap is left as it is.
 * @param bitmap Required.
 */
TB_API void tb_bitmap_unpremultiply(tb_bitmap *bitmap);

/**
 * @brief Say whether a bitmap's pixels are premultiplied, changing none of
 * them: for pixels wrapped from elsewhere.
 * @param bitmap Required; not gray when `premultiplied` is true.
 * @param premultiplied What the pixels are.
 */
TB_API void tb_bitmap_set_premultiplied(tb_bitmap *bitmap, bool premultiplied);

/**
 * @brief Make every pixel of one color transparent: the color key of a
 * classic sprite sheet. Pixels whose red, green and blue are the key's,
 * whatever their alpha, become transparent black, so that a filter that
 * blends them with their neighbours adds no trace of the key.
 * @param bitmap Required; RGBA8 or BGRA8.
 * @param key The color; its alpha is not compared.
 * @return How many pixels were changed.
 */
TB_API size_t tb_bitmap_color_key(tb_bitmap *bitmap, tb_bitmap_color key);

/* -------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/** @brief What reading or writing a file came to. */
typedef enum tb_bitmap_result
{
    TB_BITMAP_OK,             /**< Done. */
    TB_BITMAP_INVALID,        /**< An argument is malformed: a bitmap no file can hold. */
    TB_BITMAP_NO_MEMORY,      /**< There is no memory for the pixels a file has. */
    TB_BITMAP_TOO_LARGE,      /**< A file, or its dimensions, are over the limits. */
    TB_BITMAP_UNKNOWN_FORMAT, /**< The bytes are not of any format the engine reads. */
    /** The file is of a format the engine reads, in a form it does not, such
     * as a BMP file holding a PNG. */
    TB_BITMAP_UNSUPPORTED,
    TB_BITMAP_MALFORMED,    /**< The file is damaged; the error says where and why. */
    TB_BITMAP_NOT_FOUND,    /**< The VFS has no such file. */
    TB_BITMAP_INPUT_FAILED, /**< The stream or the VFS could not be read. */
    TB_BITMAP_OUTPUT_FAILED /**< The stream refused what was written. */
} tb_bitmap_result;

/** @brief The largest dimension decoding allows when the options give none. */
#define TB_BITMAP_DEFAULT_MAX_SIDE ((uint32_t)16384)

/** @brief The most pixels decoding allows when the options give none: 64 Mi,
 * 256 MiB in RGBA8, as an image of 8192 by 8192. */
#define TB_BITMAP_DEFAULT_MAX_PIXELS ((uint64_t)1 << 26)

/** @brief The most bytes a stream may hold when the options give none: 256 MiB. */
#define TB_BITMAP_DEFAULT_MAX_BYTES ((uint64_t)1 << 28)

/** @brief How to decode. Zero-initialize for the defaults. */
typedef struct tb_bitmap_decode_options
{
    uint32_t max_side;   /**< Pixels across, and down; 0 for the default. */
    uint64_t max_pixels; /**< Pixels in all; 0 for the default. */
    uint64_t max_bytes;  /**< Bytes a stream or a VFS file may hold; 0 for the default. */
    const char *name;    /**< The file's name, for messages; NULL for none. */
} tb_bitmap_decode_options;

/** @brief Bytes of the message of #tb_bitmap_error. */
#define TB_BITMAP_ERROR_CAPACITY 256

/** @brief Why reading or writing a file failed, and where. */
typedef struct tb_bitmap_error
{
    tb_bitmap_result result; /**< As the function returned. */
    size_t offset;           /**< The byte of the file at fault; 0 when none is. */
    const char *reason;      /**< A short phrase: "the pixels end early"; static; "" for none. */
    /** All of it, for a log: "hero.bmp: byte 54: the pixels end early". */
    char message[TB_BITMAP_ERROR_CAPACITY];
} tb_bitmap_error;

/**
 * @brief Decode an image file held in memory, of any format the engine
 * reads, recognized by its bytes: BMP, for now (see `docs/media.md`).
 * @param bytes The file; NULL only at length 0. Not kept.
 * @param length Bytes of the file.
 * @param options How to decode, or NULL for the defaults.
 * @param allocator Where the bitmap lives, or NULL for the heap.
 * @param[out] bitmap Required; on OK an RGBA8 bitmap with straight alpha,
 * which owns its pixels; NULL on failure.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return OK, UNKNOWN_FORMAT, UNSUPPORTED, MALFORMED, TOO_LARGE, NO_MEMORY,
 * or INVALID for bad options.
 */
TB_API tb_bitmap_result tb_bitmap_decode(const void *bytes, size_t length,
                                         const tb_bitmap_decode_options *options,
                                         const tb_allocator *allocator, tb_bitmap **bitmap,
                                         tb_bitmap_error *error);

/**
 * @brief Decode an image file from a stream, read from its position to its
 * end, as tb_bitmap_decode() decodes it.
 * @param stream Required open stream that can read; borrowed, and left at its end.
 * @param options As tb_bitmap_decode(). @param allocator As tb_bitmap_decode().
 * @param[out] bitmap As tb_bitmap_decode(). @param[out] error As tb_bitmap_decode().
 * @return As tb_bitmap_decode(), or INPUT_FAILED, or TOO_LARGE for a stream
 * of more than the options' bytes.
 */
TB_API tb_bitmap_result tb_bitmap_read(tb_stream *stream, const tb_bitmap_decode_options *options,
                                       const tb_allocator *allocator, tb_bitmap **bitmap,
                                       tb_bitmap_error *error);

/**
 * @brief Decode an image file read through a VFS, as tb_bitmap_decode()
 * decodes it. Messages name the path; the options' name is not used.
 * @param vfs Required. @param path Required; the file's name in the VFS.
 * @param options As tb_bitmap_decode(). @param allocator As tb_bitmap_decode().
 * @param[out] bitmap As tb_bitmap_decode(). @param[out] error As tb_bitmap_decode().
 * @return As tb_bitmap_read(), or NOT_FOUND.
 */
TB_API tb_bitmap_result tb_bitmap_load(struct tb_vfs *vfs, const char *path,
                                       const tb_bitmap_decode_options *options,
                                       const tb_allocator *allocator, tb_bitmap **bitmap,
                                       tb_bitmap_error *error);

/** @brief The bits of a pixel of a BMP file that tb_bitmap_write_bmp() writes. */
typedef enum tb_bitmap_bmp_depth
{
    /** 32 bits, BGRA, with a version 5 header whose masks say where alpha is:
     * every pixel reads back exactly. */
    TB_BITMAP_BMP_32,
    /** 24 bits, BGR, with the plain header every reader knows: alpha is dropped. */
    TB_BITMAP_BMP_24
} tb_bitmap_bmp_depth;

/**
 * @brief Write a bitmap as a BMP file, rows from the bottom as is usual. A
 * premultiplied bitmap is written straight, as BMP files are; a gray one as
 * its gray in each color channel.
 * @param bitmap Required.
 * @param depth The bits of a pixel.
 * @param sink Required open stream that can write; borrowed.
 * @param[out] error Why it failed; NULL when nobody asks.
 * @return OK; INVALID for a bitmap of no pixels, which no BMP file holds;
 * or OUTPUT_FAILED.
 */
TB_API tb_bitmap_result tb_bitmap_write_bmp(const tb_bitmap *bitmap, tb_bitmap_bmp_depth depth,
                                            tb_stream *sink, tb_bitmap_error *error);

/** @brief A short name of a result, such as "malformed"; "unknown result"
 * for a value that is none. */
TB_API const char *tb_bitmap_result_name(tb_bitmap_result result);

#endif
