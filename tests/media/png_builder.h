/* PNG files built in tests, chunk by chunk, with the engine's DEFLATE for
 * their image data and CRCs computed as they are added. */
#ifndef TB_PNG_BUILDER_H
#define TB_PNG_BUILDER_H

#include <stddef.h>
#include <stdint.h>

/* Room for every file a case builds. */
#define PNG_BUILD_CAPACITY ((size_t)16384)

typedef struct png_build
{
    uint8_t bytes[PNG_BUILD_CAPACITY];
    size_t length;
} png_build;

/* Start a file with the signature. */
void png_build_begin(png_build *file);

/* Add a chunk of a name of four letters, with its CRC. */
void png_build_chunk(png_build *file, const char *name, const void *data, size_t length);

/* Add an IHDR chunk; compression and filter method 0. */
void png_build_header(png_build *file, uint32_t width, uint32_t height, uint32_t depth,
                      uint32_t color, uint32_t interlace);

/* Add filtered rows, each its filter type and bytes, compressed in zlib
 * framing, as one IDAT chunk, or as chunks of `split` bytes when it is not 0. */
void png_build_data(png_build *file, const uint8_t *rows, size_t length, size_t split);

/* Add the IEND chunk. */
void png_build_end(png_build *file);

/* Compute again the CRC of every chunk whose length fits in the file: after
 * damage, so that damage reaches more than the CRC check. */
void png_build_fix_crcs(png_build *file);

/* The offset of the first chunk of a name, or SIZE_MAX if there is none. */
size_t png_build_find(const png_build *file, const char *name);

/* The 13-byte header of a file, as written at a chunk's data, for the tests
 * that change it. */
#define PNG_BUILD_IHDR_DATA ((size_t)16)

#endif
