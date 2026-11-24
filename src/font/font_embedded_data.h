/* The types of the fonts' data that the build generates; see
 * font_embedded.h. Generated sources include this header alone, so it
 * includes nothing of the engine's. Private to src/font/. */
#ifndef TB_FONT_EMBEDDED_DATA_H
#define TB_FONT_EMBEDDED_DATA_H

#include <stddef.h>

/* A font's file, compressed as a zlib stream (RFC 1950). */
typedef struct tb_font_blob
{
    const unsigned char *data; /* The zlib stream. */
    size_t data_length;        /* Bytes of `data`. */
    size_t length;             /* Bytes of the file it decompresses to. */
} tb_font_blob;

/* A file of the pack, and its data if this build holds it. */
typedef struct tb_font_index_entry
{
    const char *file;         /* Such as "sans/Inter-Regular.ttf". */
    const tb_font_blob *blob; /* NULL when the build leaves the file out. */
} tb_font_index_entry;

/* The generated index: one entry for every file of the pack. */
extern const tb_font_index_entry tb_font_embedded_index[];
extern const size_t tb_font_embedded_index_count;

#endif
