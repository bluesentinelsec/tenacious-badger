/* Exercise the operating system provider through the public API. */
#include "badger/assert.h"
#include "badger/random.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

TB_TEST(platform_random, an_empty_request_succeeds)
{
    TB_TEST_EXPECT(tb_random_bytes(NULL, 0));
}

TB_TEST(platform_random, a_draw_preserves_the_surrounding_bytes)
{
    unsigned char bytes[4101] = {0};
    for (size_t length = 1; length + 2 <= sizeof(bytes); length = length * 3 + 1)
    {
        TB_TEST_CONTEXT("length %zu", length);
        memset(bytes, 0xA5, sizeof(bytes));
        TB_TEST_REQUIRE(tb_random_bytes(bytes + 1, length));
        TB_TEST_EXPECT_UINT(bytes[0], ==, 0xA5);
        TB_TEST_EXPECT_UINT(bytes[length + 1], ==, 0xA5);
    }
}

TB_TEST(platform_random, two_draws_differ)
{
    uint8_t first[32] = {0};
    uint8_t second[32] = {0};
    TB_TEST_REQUIRE(tb_random_bytes(first, sizeof(first)));
    TB_TEST_REQUIRE(tb_random_bytes(second, sizeof(second)));
    TB_TEST_EXPECT(memcmp(first, second, sizeof(first)) != 0);
}

/* Each of the 8 bit positions is set in about half of 1 MiB: the count's
 * standard deviation is 512, and the bound is 12 of them. */
TB_TEST(platform_random, every_bit_is_set_about_half_the_time)
{
    enum
    {
        SIZE = 1 << 20
    };
    static unsigned char bytes[SIZE];
    size_t counts[8] = {0};
    TB_TEST_REQUIRE(tb_random_bytes(bytes, sizeof(bytes)));
    for (size_t index = 0; index < sizeof(bytes); index++)
    {
        for (size_t bit = 0; bit < 8; bit++)
        {
            counts[bit] += ((unsigned)bytes[index] >> bit) & 1u;
        }
    }
    for (size_t bit = 0; bit < 8; bit++)
    {
        TB_TEST_CONTEXT("bit %zu", bit);
        TB_TEST_EXPECT_UINT(counts[bit], >, SIZE / 2 - 6144);
        TB_TEST_EXPECT_UINT(counts[bit], <, SIZE / 2 + 6144);
    }
}

typedef struct draw
{
    uint8_t bytes[32];
    bool filled;
} draw;

static void draw_bytes(void *context)
{
    draw *const result = context;
    result->filled = tb_random_bytes(result->bytes, sizeof(result->bytes));
}

/* Threads draw at once, and no two draws are equal. */
TB_TEST(platform_random, threads_draw_at_once)
{
    enum
    {
        THREADS = 8
    };
    tb_thread threads[THREADS] = {0};
    draw draws[THREADS] = {0};
    for (size_t index = 0; index < THREADS; index++)
    {
        tb_thread_create(&threads[index], draw_bytes, &draws[index]);
    }
    for (size_t index = 0; index < THREADS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT(draws[index].filled);
    }
    for (size_t first = 0; first < THREADS; first++)
    {
        for (size_t second = first + 1; second < THREADS; second++)
        {
            TB_TEST_EXPECT(memcmp(draws[first].bytes, draws[second].bytes, 32) != 0);
        }
    }
}

static void random_contract(void *context)
{
    (void)context;
    (void)tb_random_bytes(NULL, 1);
}

TB_TEST(platform_random, a_null_buffer_with_a_length_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    TB_TEST_EXPECT_TRAP(random_contract, NULL, "assertion failed");
}
