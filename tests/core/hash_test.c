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

TB_TEST(core_hash, bytes_are_hashed_with_xxh64)
{
    typedef struct vector_row
    {
        const char *text;
        uint64_t hash;
    } vector_row;
    /* Published results of XXH64 with a seed of zero. */
    static const vector_row rows[] = {
        {"", UINT64_C(0xEF46DB3751D8E999)},
        {"a", UINT64_C(0xD24EC4F1A98C6E5B)},
        {"abc", UINT64_C(0x44BC2CF5AD770999)},
        {"xxhash", UINT64_C(0x32DD38952C4BC720)},
        {"Nobody inspects the spammish repetition", UINT64_C(0xFBCEA83C8A378BF1)},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("\"%s\"", rows[index].text);
        TB_TEST_EXPECT_HEX(tb_hash_bytes(rows[index].text, strlen(rows[index].text)), ==,
                           rows[index].hash);
        TB_TEST_EXPECT_HEX(tb_hash_string(rows[index].text), ==, rows[index].hash);
    }
    TB_TEST_EXPECT_HEX(tb_hash_bytes(NULL, 0), ==, UINT64_C(0xEF46DB3751D8E999));
}

/* The function takes its input 32 bytes at a time, then 8, then 4, then 1.
 * These lengths sit on and beside each of those steps, so that every path
 * through it is pinned. The expected values come from a second implementation
 * written from the specification, which agrees with the published results
 * above. Byte `index` of the input is `index * 7 + 3`. */
TB_TEST(core_hash, every_length_takes_its_own_path_to_a_known_result)
{
    typedef struct length_row
    {
        size_t length;
        uint64_t hash;
    } length_row;
    static const length_row rows[] = {
        {1, UINT64_C(0x1F25C8D0BC1F4BB6)},  {3, UINT64_C(0x31D2363F52E564C9)},
        {4, UINT64_C(0x9BB64B7D66EE9FDA)},  {7, UINT64_C(0x9A7B149959CE60D8)},
        {8, UINT64_C(0xDAB99D95C6F90092)},  {15, UINT64_C(0x1B47CB8243CC8E32)},
        {16, UINT64_C(0x434850232B787BE2)}, {31, UINT64_C(0xA2AA5F33CC4A6119)},
        {32, UINT64_C(0x23C3C17EF790FD97)}, {33, UINT64_C(0x50A7CFC7BA588784)},
        {63, UINT64_C(0x5E3E54B431C7493C)}, {64, UINT64_C(0x0EB64B3EF6EEB01F)},
        {65, UINT64_C(0xA383B724B2BD12F1)}, {100, UINT64_C(0xA61F8D4C170FE531)},
    };
    unsigned char bytes[100] = {0};

    for (size_t index = 0; index < sizeof(bytes); index++)
    {
        bytes[index] = (unsigned char)((index * 7 + 3) & 0xFFu);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("%zu bytes", rows[row].length);
        TB_TEST_EXPECT_HEX(tb_hash_bytes(bytes, rows[row].length), ==, rows[row].hash);
    }
}

/* The words of the input are copied out of it, never read in place, so where
 * the input is in memory cannot matter: it hashes the same at every alignment. */
TB_TEST(core_hash, the_hash_does_not_depend_on_where_the_bytes_are)
{
    enum
    {
        LENGTH = 77,
        OFFSET_COUNT = 9
    };
    unsigned char buffer[LENGTH + OFFSET_COUNT] = {0};
    uint64_t expected = 0;

    for (size_t offset = 0; offset < OFFSET_COUNT; offset++)
    {
        TB_TEST_CONTEXT("offset %zu", offset);
        for (size_t index = 0; index < LENGTH; index++)
        {
            buffer[offset + index] = (unsigned char)((index * 31 + 5) & 0xFFu);
        }
        if (offset == 0)
        {
            expected = tb_hash_bytes(buffer, LENGTH);
        }
        TB_TEST_EXPECT_HEX(tb_hash_bytes(buffer + offset, LENGTH), ==, expected);
    }
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
