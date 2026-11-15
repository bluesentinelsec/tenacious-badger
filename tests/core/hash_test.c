/* The hash functions are named algorithms with published results, so their
 * output is pinned to those results: a table that persists hashes, or a tool
 * on another platform, must see the same numbers. Their job is to spread
 * keys, which is checked on the kinds of key the engine has: short names,
 * consecutive integers, and aligned addresses. */
#include "badger/hash.h"

#include "badger/assert.h"
#include "tb_test.h"

#include <stdio.h>
#include <string.h>

TB_TEST(core_hash, bytes_are_hashed_with_fnv_1a)
{
    typedef struct vector_row
    {
        const char *text;
        uint64_t hash;
    } vector_row;
    /* Published test vectors for 64-bit FNV-1a. */
    static const vector_row rows[] = {
        {"", UINT64_C(0xCBF29CE484222325)},       {"a", UINT64_C(0xAF63DC4C8601EC8C)},
        {"b", UINT64_C(0xAF63DF4C8601F1A5)},      {"abc", UINT64_C(0xE71FA2190541574B)},
        {"foobar", UINT64_C(0x85944171F73967E8)},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("\"%s\"", rows[index].text);
        TB_TEST_EXPECT_HEX(tb_hash_bytes(rows[index].text, strlen(rows[index].text)), ==,
                           rows[index].hash);
        TB_TEST_EXPECT_HEX(tb_hash_string(rows[index].text), ==, rows[index].hash);
    }
    TB_TEST_EXPECT_HEX(tb_hash_bytes(NULL, 0), ==, UINT64_C(0xCBF29CE484222325));
}

TB_TEST(core_hash, every_byte_and_the_length_change_the_hash)
{
    static const char with_zero[] = {'a', '\0', 'b'};
    char name[] = "player_health";
    const uint64_t original = tb_hash_string(name);

    for (size_t index = 0; index < sizeof(name) - 1; index++)
    {
        const char saved = name[index];

        name[index] = 'X';
        TB_TEST_CONTEXT("byte %zu", index);
        TB_TEST_EXPECT_HEX(tb_hash_string(name), !=, original);
        name[index] = saved;
    }
    /* A prefix, and bytes after a zero byte, are different keys. */
    TB_TEST_EXPECT_HEX(tb_hash_bytes(name, 6), !=, original);
    TB_TEST_EXPECT_HEX(tb_hash_bytes(with_zero, 3), !=, tb_hash_bytes(with_zero, 1));
    TB_TEST_EXPECT_HEX(tb_hash_bytes(with_zero, 2), !=, tb_hash_bytes(with_zero, 1));
}

TB_TEST(core_hash, integers_are_mixed_with_the_splitmix64_finalizer)
{
    /* SplitMix64 seeded with zero adds this constant and then applies the
     * finalizer, so its published first outputs are the finalizer of the
     * constant, of twice it, and of three times it. */
    const uint64_t increment = UINT64_C(0x9E3779B97F4A7C15);

    TB_TEST_EXPECT_HEX(tb_hash_u64(increment), ==, UINT64_C(0xE220A8397B1DCDAF));
    TB_TEST_EXPECT_HEX(tb_hash_u64(increment * 2), ==, UINT64_C(0x6E789E6AA1B965F4));
    TB_TEST_EXPECT_HEX(tb_hash_u64(increment * 3), ==, UINT64_C(0x06C45D188009454F));
    TB_TEST_EXPECT_HEX(tb_hash_u64(0), ==, 0);
}

enum
{
    BUCKET_COUNT = 1024,
    KEY_COUNT = 102400,
    EXPECTED_PER_BUCKET = KEY_COUNT / BUCKET_COUNT
};

/* Checks that a table of BUCKET_COUNT slots would be filled evenly, with the
 * chi-squared statistic: the squared distance of each slot from its share of
 * the keys, summed over the slots and divided by that share. Keys spread by
 * chance score close to the number of slots, give or take 45. A hash that
 * leaves slots unused, or favors some, scores many times more. The keys of
 * each case are fixed, so its score is the same on every run. */
static void expect_even_spread(const size_t *buckets)
{
    enum
    {
        /* Six times the 45 that chance allows. */
        LARGEST_FAIR_SCORE = BUCKET_COUNT + 270
    };
    size_t squared_distance = {0};

    for (size_t index = 0; index < BUCKET_COUNT; index++)
    {
        size_t distance = {0};

        if (buckets[index] > EXPECTED_PER_BUCKET)
        {
            distance = buckets[index] - EXPECTED_PER_BUCKET;
        }
        else
        {
            distance = EXPECTED_PER_BUCKET - buckets[index];
        }
        squared_distance += distance * distance;
    }
    TB_TEST_EXPECT_UINT(squared_distance / EXPECTED_PER_BUCKET, <, LARGEST_FAIR_SCORE);
}

TB_TEST(core_hash, consecutive_integers_spread_over_both_ends_of_the_hash)
{
    static size_t low_buckets[BUCKET_COUNT] = {0};
    static size_t high_buckets[BUCKET_COUNT] = {0};

    memset(low_buckets, 0, sizeof(low_buckets));
    memset(high_buckets, 0, sizeof(high_buckets));
    for (uint64_t key = 0; key < KEY_COUNT; key++)
    {
        const uint64_t hash = tb_hash_u64(key);

        low_buckets[hash % BUCKET_COUNT]++;
        high_buckets[hash >> 54]++;
    }
    expect_even_spread(low_buckets);
    expect_even_spread(high_buckets);
}

TB_TEST(core_hash, aligned_addresses_spread_over_the_low_bits)
{
    static size_t buckets[BUCKET_COUNT] = {0};
    static char storage[16] = {0};
    /* Real addresses change from one run to the next, and a test must not:
     * these are made from a fixed number and are only hashed, never used. */
    const uintptr_t first_address = 0x40001000u;

    memset(buckets, 0, sizeof(buckets));
    /* Addresses sixteen bytes apart, as an allocator hands them out: their
     * low four bits are all the same. */
    for (uintptr_t index = 0; index < KEY_COUNT; index++)
    {
        const void *const pointer = (const void *)(first_address + index * 16);

        buckets[tb_hash_pointer(pointer) % BUCKET_COUNT]++;
    }
    expect_even_spread(buckets);
    TB_TEST_EXPECT_HEX(tb_hash_pointer(storage), ==, tb_hash_u64((uint64_t)(uintptr_t)storage));
    TB_TEST_EXPECT_HEX(tb_hash_pointer(NULL), ==, tb_hash_u64(0));
}

TB_TEST(core_hash, short_names_spread_over_the_low_bits)
{
    static size_t buckets[BUCKET_COUNT] = {0};

    memset(buckets, 0, sizeof(buckets));
    for (size_t index = 0; index < KEY_COUNT; index++)
    {
        char name[32] = {0};

        (void)snprintf(name, sizeof(name), "entity_%zu", index);
        buckets[tb_hash_string(name) % BUCKET_COUNT]++;
    }
    expect_even_spread(buckets);
}

static void hash_null_bytes(void *context)
{
    (void)context;
    (void)tb_hash_bytes(NULL, 4);
}

static void hash_null_string(void *context)
{
    (void)context;
    (void)tb_hash_string(NULL);
}

TB_TEST(core_hash, missing_bytes_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(hash_null_bytes, NULL, "bytes != NULL || length == 0");
    TB_TEST_EXPECT_TRAP(hash_null_string, NULL, "string != NULL");
}
