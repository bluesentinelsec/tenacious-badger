#include "internal.h"

/* SplitMix64 (Steele, Lea, Flood): one 64-bit state word, full period, and
 * well-distributed output even from adjacent seeds. */
static const uint64_t TB_TEST_SPLITMIX_INCREMENT = UINT64_C(0x9E3779B97F4A7C15);
static const uint64_t TB_TEST_SPLITMIX_MULTIPLIER_A = UINT64_C(0xBF58476D1CE4E5B9);
static const uint64_t TB_TEST_SPLITMIX_MULTIPLIER_B = UINT64_C(0x94D049BB133111EB);

/* FNV-1a, 64-bit. */
static const uint64_t TB_TEST_FNV_OFFSET_BASIS = UINT64_C(0xCBF29CE484222325);
static const uint64_t TB_TEST_FNV_PRIME = UINT64_C(0x00000100000001B3);

void tb_test_generator_seed(tb_test_generator *generator, uint64_t seed)
{
    generator->state = seed;
}

uint64_t tb_test_generator_next(tb_test_generator *generator)
{
    uint64_t mixed = {0};

    generator->state += TB_TEST_SPLITMIX_INCREMENT;
    mixed = generator->state;
    mixed = (mixed ^ (mixed >> 30)) * TB_TEST_SPLITMIX_MULTIPLIER_A;
    mixed = (mixed ^ (mixed >> 27)) * TB_TEST_SPLITMIX_MULTIPLIER_B;
    return mixed ^ (mixed >> 31);
}

uint64_t tb_test_generator_below(tb_test_generator *generator, uint64_t bound)
{
    /* 2^64 is rarely a multiple of the bound, so the lowest values would be
     * slightly more likely. Reject the first (2^64 mod bound) draws instead. */
    const uint64_t rejected_below = (UINT64_MAX - bound + 1) % bound;
    uint64_t draw = {0};

    draw = tb_test_generator_next(generator);
    while (draw < rejected_below)
    {
        draw = tb_test_generator_next(generator);
    }
    return draw % bound;
}

static uint64_t tb_test_hash_text(uint64_t hash, const char *text)
{
    for (const char *cursor = text; *cursor != '\0'; cursor++)
    {
        hash ^= (uint64_t)(unsigned char)*cursor;
        hash *= TB_TEST_FNV_PRIME;
    }
    return hash;
}

uint64_t tb_test_generator_seed_for_case(uint64_t run_seed, const char *suite, const char *name)
{
    tb_test_generator mixer = TB_TEST_GENERATOR_INIT;
    uint64_t hash = {0};

    hash = TB_TEST_FNV_OFFSET_BASIS;
    hash = tb_test_hash_text(hash, suite);
    hash = tb_test_hash_text(hash, ".");
    hash = tb_test_hash_text(hash, name);
    tb_test_generator_seed(&mixer, run_seed ^ hash);
    return tb_test_generator_next(&mixer);
}
