/* The cache that decompresses each embedded font once: many threads that
 * ask for one font at the same moment, the first time, all get the same
 * bytes, decompressed once. The engine's own cache decompresses a font only
 * the first time in a process, so the race is run on caches of the test's
 * own, with data the test compresses, in a build of any TB_EMBED_FONTS. */
#include "badger/deflate.h"
#include "badger/memory.h"
#include "badger/thread.h"
#include "font/font_embedded.h"
#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define THREAD_COUNT 8
#define ROUNDS 16
#define FILE_LENGTH ((size_t)200000)
#define SLOT ((size_t)7)

typedef struct racer
{
    tb_font_embedded_cache *cache;
    const tb_font_blob *blob;
    tb_semaphore *start;
    const void *bytes;
} racer;

static void race(void *argument)
{
    racer *self = argument;
    tb_semaphore_wait(self->start);
    self->bytes = tb_font_embedded_cache_get(self->cache, SLOT, self->blob);
}

/* A file that compresses, but not to nothing. */
static uint8_t *make_file(void)
{
    uint8_t *file = tb_malloc(FILE_LENGTH);
    uint32_t state = 12345;
    for (size_t index = 0; index < FILE_LENGTH; index++)
    {
        state = state * 1103515245u + 12345u;
        file[index] = (uint8_t)(index % 64 < 32 ? index % 7 : (state >> 24) % 16);
    }
    return file;
}

static tb_font_blob compress(const uint8_t *file, uint8_t **data)
{
    size_t capacity = tb_deflate_bound(FILE_LENGTH);
    tb_font_blob blob = {0};
    size_t written = 0;
    *data = tb_malloc(capacity);
    TB_TEST_REQUIRE_UINT(tb_deflate(TB_DEFLATE_ZLIB, TB_DEFLATE_BEST_LEVEL, file, FILE_LENGTH,
                                    *data, capacity, &written),
                         ==, TB_DEFLATE_OK);
    blob.data = *data;
    blob.data_length = written;
    blob.length = FILE_LENGTH;
    return blob;
}

TB_TEST(font_embedded, threads_that_ask_at_once_share_one_decompression)
{
    uint8_t *file = make_file();
    uint8_t *data = NULL;
    const tb_font_blob blob = compress(file, &data);
    tb_font_embedded_cache *cache = tb_calloc(1, sizeof *cache);
    for (size_t round = 0; round < ROUNDS; round++)
    {
        tb_semaphore start = {0};
        tb_thread threads[THREAD_COUNT] = {0};
        racer racers[THREAD_COUNT] = {0};
        TB_TEST_CONTEXT("round %zu", round);
        for (size_t index = 0; index < THREAD_COUNT; index++)
        {
            racers[index] = (racer){cache, &blob, &start, NULL};
            tb_thread_create(&threads[index], race, &racers[index]);
        }
        tb_semaphore_signal(&start, THREAD_COUNT);
        for (size_t index = 0; index < THREAD_COUNT; index++)
        {
            tb_thread_join(&threads[index]);
        }
        for (size_t index = 0; index < THREAD_COUNT; index++)
        {
            TB_TEST_EXPECT_POINTER(racers[index].bytes, ==, racers[0].bytes);
        }
        TB_TEST_REQUIRE_POINTER(racers[0].bytes, !=, NULL);
        TB_TEST_EXPECT_BYTES(racers[0].bytes, file, FILE_LENGTH);
        /* The other slots were not touched. */
        TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&cache->bytes[SLOT + 1]), ==, NULL);
        tb_font_embedded_cache_release(cache);
        TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&cache->bytes[SLOT]), ==, NULL);
    }
    tb_free(cache);
    tb_free(data);
    tb_free(file);
}

TB_TEST(font_embedded, a_font_asked_for_again_is_not_decompressed_again)
{
    uint8_t *file = make_file();
    uint8_t *data = NULL;
    tb_font_blob blob = compress(file, &data);
    tb_font_embedded_cache *cache = tb_calloc(1, sizeof *cache);
    const void *first = tb_font_embedded_cache_get(cache, SLOT, &blob);
    /* Were it decompressed again, the damaged stream would end the process. */
    data[blob.data_length / 2] ^= 0xFF;
    const void *second = tb_font_embedded_cache_get(cache, SLOT, &blob);
    TB_TEST_EXPECT_POINTER(first, ==, second);
    TB_TEST_EXPECT_BYTES(second, file, FILE_LENGTH);
    tb_font_embedded_cache_release(cache);
    /* Releasing a cache with nothing in it does nothing. */
    tb_font_embedded_cache_release(cache);
    tb_free(cache);
    tb_free(data);
    tb_free(file);
}
