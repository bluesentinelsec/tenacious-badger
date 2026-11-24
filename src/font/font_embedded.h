/* The fonts the engine carries: the compressed data the build generates, and
 * the cache that decompresses each font once. Private to src/font/.
 *
 * The build (cmake/EmbedFonts.cmake) compresses each `.ttf` file of
 * resources/fonts/ into a C source of its own, with badger-fontpack, and
 * writes an index of every file of the pack, in the order of their names:
 * an entry holds the file's name, and its compressed data, or NULL when the
 * build leaves the file out (TB_EMBED_FONTS). font_embedded.c describes the
 * same files, in the same order, by hand. */
#ifndef TB_FONT_EMBEDDED_H
#define TB_FONT_EMBEDDED_H

#include "badger/platform.h"
#include "badger/thread.h"
#include "badger/virtual_memory.h"
#include "font_embedded_data.h"

#include <stddef.h>

/* Files of the pack that font_embedded.c describes: the most the index holds. */
#define TB_FONT_EMBEDDED_FILE_COUNT ((size_t)60)

/* The decompressed fonts: a slot for each file of the pack, filled once.
 * Zero-initialize it. The engine's own cache lives for the process; a test
 * may make one of its own and release it. */
typedef struct tb_font_embedded_cache
{
    tb_mutex mutex; /* Held while a font is decompressed. */
    /* The bytes of each file once decompressed; NULL before. */
    tb_platform_atomic_pointer bytes[TB_FONT_EMBEDDED_FILE_COUNT];
    tb_vm_region regions[TB_FONT_EMBEDDED_FILE_COUNT]; /* Where they live. */
} tb_font_embedded_cache;

/* The bytes of a file of the pack, decompressed into `cache` the first time
 * they are asked for. `slot` is the file's index in the pack, and `blob` its
 * data. Thread-safe. Running out of memory, or a stream that does not
 * decompress to its length, ends the process. */
const void *tb_font_embedded_cache_get(tb_font_embedded_cache *cache, size_t slot,
                                       const tb_font_blob *blob);

/* Release every font of a cache that no thread uses any more, and zero it. */
void tb_font_embedded_cache_release(tb_font_embedded_cache *cache);

#endif
