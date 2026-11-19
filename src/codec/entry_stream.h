/* A stream of one entry of an archive or a package, shared by their readers:
 * it reads through layers of streams it owns (a range of the source, then
 * whatever undoes compression), ends after the entry's size,
 * and checks its CRC-32. */
#ifndef TB_CODEC_ENTRY_STREAM_H
#define TB_CODEC_ENTRY_STREAM_H

#include "badger/allocator.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

/* The most layers an entry has: a range, then decompression. */
#define TB_ENTRY_MAX_LAYERS 2u

/* Open streams, from the source outward: each reads the one before it. */
typedef struct tb_entry_layers
{
    tb_stream layers[TB_ENTRY_MAX_LAYERS];
    size_t count;
} tb_entry_layers;

/* Open a stream of the outermost layer's bytes that ends after `size` of
 * them. Data that ends early, runs long, fails to decode, or has another
 * CRC-32 is read as TB_STREAM_MALFORMED, at the latest when the size is
 * reached. The stream takes the layers and closes them, outermost first,
 * when it is closed. */
void tb_entry_stream_open(const tb_entry_layers *layers, uint64_t size, uint32_t crc32,
                          const tb_allocator *allocator, tb_stream *stream);

/* Close layers that were never given to an entry stream, outermost first. */
void tb_entry_layers_close(tb_entry_layers *layers);

#endif
