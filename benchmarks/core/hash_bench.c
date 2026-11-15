/* Hashing bytes: keys of fixed lengths, and keys whose lengths vary the way
 * identifiers do, which is what tables of names hash. */
#include "tb_bench.h"

#include "badger/hash.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>

/* Keys are taken from a pool of bytes at scattered offsets. */
#define HASH_POOL_SIZE ((size_t)1 << 20)
#define HASH_KEY_COUNT ((size_t)4096)
#define HASH_LONGEST_KEY ((size_t)4096)

typedef struct hash_keys
{
    const unsigned char *pool;
    size_t offsets[HASH_KEY_COUNT];
    size_t lengths[HASH_KEY_COUNT];
    size_t rounds;
} hash_keys;

static void run_bytes(void *context)
{
    const hash_keys *const keys = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        for (size_t index = 0; index < HASH_KEY_COUNT; index++)
        {
            sum += tb_hash_bytes(keys->pool + keys->offsets[index], keys->lengths[index]);
        }
    }
    tb_bench_keep(sum);
}

static void run_integers(void *context)
{
    const hash_keys *const keys = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        for (size_t index = 0; index < HASH_KEY_COUNT; index++)
        {
            sum += tb_hash_u64(keys->offsets[index] + round);
        }
    }
    tb_bench_keep(sum);
}

/* Measures keys whose lengths are spread evenly from `shortest` to `longest`. */
static void measure_lengths(tb_bench *bench, hash_keys *keys, const char *name, size_t shortest,
                            size_t longest, size_t full_rounds)
{
    uint64_t state = 99;
    tb_bench_case measured = {.name = name, .variant = "badger", .run = run_bytes, .context = keys};

    for (size_t index = 0; index < HASH_KEY_COUNT; index++)
    {
        keys->lengths[index] =
            shortest + (size_t)(tb_bench_random(&state) % (longest - shortest + 1));
        keys->offsets[index] = (size_t)(tb_bench_random(&state) % (HASH_POOL_SIZE - longest));
    }
    keys->rounds = tb_bench_size(bench, full_rounds * HASH_KEY_COUNT) / HASH_KEY_COUNT;
    if (keys->rounds == 0)
    {
        keys->rounds = 1;
    }
    measured.operations = (double)HASH_KEY_COUNT * (double)keys->rounds;
    tb_bench_measure(bench, "hash", &measured, 1);
}

void tb_bench_hash(tb_bench *bench)
{
    hash_keys *keys = NULL;
    unsigned char *pool = NULL;
    uint64_t state = 5;
    tb_bench_case integers = {
        .name = "hash a 64-bit integer", .variant = "badger", .run = run_integers};

    if (!tb_bench_wants(bench, "hash"))
    {
        return;
    }
    keys = tb_calloc(1, sizeof(*keys));
    pool = tb_malloc(HASH_POOL_SIZE);
    for (size_t index = 0; index < HASH_POOL_SIZE; index++)
    {
        pool[index] = (unsigned char)(tb_bench_random(&state) & 0xFFu);
    }
    keys->pool = pool;
    measure_lengths(bench, keys, "hash 8 bytes", 8, 8, 200);
    measure_lengths(bench, keys, "hash 32 bytes", 32, 32, 200);
    measure_lengths(bench, keys, "hash an identifier of 5 to 40 bytes", 5, 40, 200);
    measure_lengths(bench, keys, "hash 4,096 bytes", HASH_LONGEST_KEY, HASH_LONGEST_KEY, 4);
    /* Measured last, with the many rounds of the short keys. */
    measure_lengths(bench, keys, "hash 16 bytes", 16, 16, 200);
    integers.context = keys;
    integers.operations = (double)HASH_KEY_COUNT * (double)keys->rounds;
    tb_bench_measure(bench, "hash", &integers, 1);
    tb_free(pool);
    tb_free(keys);
}
