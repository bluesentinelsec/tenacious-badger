/* Random bytes cannot be checked for being unpredictable, only for what
 * unpredictable bytes are never far from: every byte of the buffer is
 * written, two draws differ, and every bit is set about half the time. The
 * bounds are wide enough that a correct generator fails them about once in
 * 10^20 runs. */
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

/* Every byte is written: a buffer filled from two opposite patterns comes
 * back equal in neither, at any length and alignment. */
TB_TEST(platform_random, every_byte_of_the_buffer_is_written)
{
    static unsigned char zeros[4099];
    static unsigned char ones[4099];
    for (size_t length = 1; length <= 4099; length = length * 3 + 1)
    {
        size_t untouched = 0;
        TB_TEST_CONTEXT("length %zu", length);
        memset(zeros, 0x00, sizeof(zeros));
        memset(ones, 0xFF, sizeof(ones));
        TB_TEST_REQUIRE(tb_random_bytes(zeros, length));
        TB_TEST_REQUIRE(tb_random_bytes(ones, length));
        /* A byte left alone would be 0x00 in one and 0xFF in the other. */
        for (size_t index = 0; index < length; index++)
        {
            untouched += zeros[index] == 0x00 && ones[index] == 0xFF ? 1u : 0u;
        }
        /* By chance, a written byte matches both patterns 1 time in 65,536. */
        TB_TEST_EXPECT_UINT(untouched, <=, 2);
        TB_TEST_EXPECT_UINT(zeros[length], ==, 0x00);
        TB_TEST_EXPECT_UINT(ones[length], ==, 0xFF);
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
            counts[bit] += (bytes[index] >> bit) & 1u;
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
