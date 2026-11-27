/* What the bitmap's sources share: the structure, and making a bitmap whose
 * allocation may fail, for decoders of untrusted files. */
#ifndef TB_BITMAP_INTERNAL_H
#define TB_BITMAP_INTERNAL_H

#include "badger/bitmap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_bitmap
{
    tb_allocator allocator; /* Where the bitmap, and pixels it owns, live. */
    uint8_t *pixels;        /* The top row; NULL when there are no pixels. */
    size_t stride;          /* Bytes from a row to the next. */
    size_t size;            /* Bytes from the first pixel to one past the last. */
    size_t capacity;        /* Bytes allocated for pixels it owns; 0 when it borrows. */
    uint32_t width;
    uint32_t height;
    tb_bitmap_format format;
    bool premultiplied;
    bool owns_pixels;
};

/* Make a bitmap that owns zeroed, packed pixels, or NULL when there is no
 * memory for them. The dimensions are within TB_BITMAP_MAX_DIMENSION. */
tb_bitmap *tb_bitmap_try_create(uint32_t width, uint32_t height, tb_bitmap_format format,
                                const tb_allocator *allocator);

/* A premultiplied color, straight: each channel divided by alpha, rounded
 * to nearest; transparent black where alpha is zero. */
tb_bitmap_color tb_bitmap_unpremultiplied_color(tb_bitmap_color color);

/* Decode a BMP file: the bytes begin with "BM". As tb_bitmap_decode(), with
 * the options resolved to their limits and `error` required. */
tb_bitmap_result tb_bitmap_decode_bmp(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error);

/* The eight bytes every PNG file begins with. */
extern const uint8_t tb_bitmap_png_signature[8];

/* Decode a PNG file: the bytes begin with its signature. As
 * tb_bitmap_decode_bmp(). */
tb_bitmap_result tb_bitmap_decode_png(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error);

/* TGA has no leading signature. Check its footer or header after the signed formats. */
bool tb_bitmap_is_tga(const uint8_t *bytes, size_t length);
tb_bitmap_result tb_bitmap_decode_tga(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error);

/* A color's luminance (Rec. 709), as tb_bitmap_convert() makes gray. */
uint8_t tb_bitmap_luminance(tb_bitmap_color color);

/* An error's offset when no byte of the file is at fault, until
 * tb_bitmap_describe() writes its message. */
#define TB_BITMAP_NO_OFFSET SIZE_MAX

/* Fill an error's result, offset (or TB_BITMAP_NO_OFFSET) and reason, and
 * return its result. */
tb_bitmap_result tb_bitmap_fail(tb_bitmap_error *error, tb_bitmap_result result, size_t offset,
                                const char *reason);

/* As tb_bitmap_fail(), for a fault in a chunk of a file: `chunk` is its
 * four letters. */
tb_bitmap_result tb_bitmap_fail_in(tb_bitmap_error *error, tb_bitmap_result result,
                                   const char *chunk, size_t offset, const char *reason);

/* Write an error's message, naming the file when `name` is not NULL, and
 * the byte at fault when there is one. */
void tb_bitmap_describe(tb_bitmap_error *error, const char *name);

#endif
