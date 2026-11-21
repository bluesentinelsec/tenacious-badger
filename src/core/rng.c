/* xoshiro256** and SplitMix64, as in the public-domain reference code by
 * David Blackman and Sebastiano Vigna (https://prng.di.unimi.it/), and
 * Lemire's method for unbiased bounded integers ("Fast Random Integer
 * Generation in an Interval", ACM TOMACS, 2019). */
#include "badger/rng.h"

#include "badger/assert.h"
#include "badger/random.h"

#include <math.h>
#include <stddef.h>

/* SplitMix64 (Steele, Lea, and Flood). */
#define TB_RNG_SPLITMIX_INCREMENT UINT64_C(0x9E3779B97F4A7C15)
#define TB_RNG_SPLITMIX_MULTIPLIER_A UINT64_C(0xBF58476D1CE4E5B9)
#define TB_RNG_SPLITMIX_MULTIPLIER_B UINT64_C(0x94D049BB133111EB)

/* The bits that the float and double conversions keep, and the weight of one
 * step of each. */
#define TB_RNG_FLOAT_BITS 24
#define TB_RNG_DOUBLE_BITS 53
#define TB_RNG_FLOAT_STEP (1.0f / 16777216.0f)        /* 2^-24 */
#define TB_RNG_DOUBLE_STEP (1.0 / 9007199254740992.0) /* 2^-53 */

/* The polynomial of a jump by 2^128 numbers, from the reference code. */
static const uint64_t tb_rng_jump_polynomial[4] = {
    UINT64_C(0x180EC6D33CFD0ABA),
    UINT64_C(0xD5A61266F0C9392C),
    UINT64_C(0xA9582618E03FC9AA),
    UINT64_C(0x39ABDC4529B1661C),
};

static uint64_t tb_rng_rotate_left(uint64_t value, unsigned bits)
{
    return (value << bits) | (value >> (64u - bits));
}

static bool tb_rng_is_seeded(const tb_rng *rng)
{
    return (rng->state[0] | rng->state[1] | rng->state[2] | rng->state[3]) != 0;
}

static uint64_t tb_rng_splitmix(uint64_t *state)
{
    uint64_t mixed = 0;

    *state += TB_RNG_SPLITMIX_INCREMENT;
    mixed = *state;
    mixed = (mixed ^ (mixed >> 30)) * TB_RNG_SPLITMIX_MULTIPLIER_A;
    mixed = (mixed ^ (mixed >> 27)) * TB_RNG_SPLITMIX_MULTIPLIER_B;
    return mixed ^ (mixed >> 31);
}

void tb_rng_seed(tb_rng *rng, uint64_t seed)
{
    uint64_t mixer = seed;

    TB_ASSERT(rng != NULL);
    /* SplitMix64 gives each of its 2^64 states a different output, so four
     * outputs in a row are never all zero, and the state is always valid. */
    for (size_t word = 0; word < 4; word++)
    {
        rng->state[word] = tb_rng_splitmix(&mixer);
    }
}

bool tb_rng_seed_from_system(tb_rng *rng, uint64_t *seed)
{
    uint64_t drawn = 0;

    TB_ASSERT(rng != NULL);
    TB_ASSERT(seed != NULL);
    if (!tb_random_bytes(&drawn, sizeof(drawn)))
    {
        return false;
    }
    tb_rng_seed(rng, drawn);
    *seed = drawn;
    return true;
}

/* The step of the generator, without the checks of the public function. */
static uint64_t tb_rng_step(tb_rng *rng)
{
    uint64_t *const state = rng->state;
    const uint64_t result = tb_rng_rotate_left(state[1] * 5, 7) * 9;
    const uint64_t shifted = state[1] << 17;

    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= shifted;
    state[3] = tb_rng_rotate_left(state[3], 45);
    return result;
}

static void tb_rng_check(const tb_rng *rng)
{
    TB_ASSERT(rng != NULL);
    TB_ASSERT_MSG(tb_rng_is_seeded(rng), "the generator is seeded");
}

uint64_t tb_rng_next(tb_rng *rng)
{
    tb_rng_check(rng);
    return tb_rng_step(rng);
}

uint32_t tb_rng_next_u32(tb_rng *rng)
{
    tb_rng_check(rng);
    return (uint32_t)(tb_rng_step(rng) >> 32);
}

/* Lemire's method in 32 bits: the upper half of a 32 by 32 bit product is
 * uniform over the bound, once the few draws whose lower half falls below
 * 2^32 mod bound are redrawn. Most bounds need no redraw at all, and the
 * remainder is computed only when one might be needed. */
static uint32_t tb_rng_below_32(tb_rng *rng, uint32_t bound)
{
    uint64_t product = (uint64_t)(uint32_t)(tb_rng_step(rng) >> 32) * bound;
    uint32_t low = (uint32_t)product;

    if (low < bound)
    {
        /* 2^32 mod bound, without a 33-bit number. */
        const uint32_t threshold = (UINT32_MAX - bound + 1u) % bound;

        while (low < threshold)
        {
            product = (uint64_t)(uint32_t)(tb_rng_step(rng) >> 32) * bound;
            low = (uint32_t)product;
        }
    }
    return (uint32_t)(product >> 32);
}

/* The 128 bits of a product of two 64-bit numbers, in two halves. */
typedef struct tb_rng_wide
{
    uint64_t high;
    uint64_t low;
} tb_rng_wide;

static tb_rng_wide tb_rng_multiply(uint64_t left, uint64_t right)
{
    const uint64_t left_low = left & UINT32_MAX;
    const uint64_t left_high = left >> 32;
    const uint64_t right_low = right & UINT32_MAX;
    const uint64_t right_high = right >> 32;
    const uint64_t low_low = left_low * right_low;
    const uint64_t high_low = left_high * right_low;
    const uint64_t low_high = left_low * right_high;
    const uint64_t high_high = left_high * right_high;
    /* No sum of 32-bit halves here can carry out of 64 bits. */
    const uint64_t middle = (low_low >> 32) + (high_low & UINT32_MAX) + low_high;
    tb_rng_wide product = {0};

    product.high = high_high + (high_low >> 32) + (middle >> 32);
    product.low = (middle << 32) | (low_low & UINT32_MAX);
    return product;
}

/* Lemire's method in 64 bits, for bounds beyond 32 bits. */
static uint64_t tb_rng_below_64(tb_rng *rng, uint64_t bound)
{
    tb_rng_wide product = tb_rng_multiply(tb_rng_step(rng), bound);

    if (product.low < bound)
    {
        /* 2^64 mod bound, without a 65-bit number. */
        const uint64_t threshold = (UINT64_MAX - bound + 1u) % bound;

        while (product.low < threshold)
        {
            product = tb_rng_multiply(tb_rng_step(rng), bound);
        }
    }
    return product.high;
}

uint64_t tb_rng_below(tb_rng *rng, uint64_t bound)
{
    tb_rng_check(rng);
    TB_ASSERT(bound >= 1);
    if (bound <= UINT32_MAX)
    {
        return tb_rng_below_32(rng, (uint32_t)bound);
    }
    return tb_rng_below_64(rng, bound);
}

/* The int64_t whose two's complement bits are `bits`, which a cast does not
 * promise for values above INT64_MAX. */
static int64_t tb_rng_to_signed(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX)
    {
        return (int64_t)bits;
    }
    return -(int64_t)(UINT64_MAX - bits) - 1;
}

int64_t tb_rng_between(tb_rng *rng, int64_t minimum, int64_t maximum)
{
    uint64_t span = 0;

    tb_rng_check(rng);
    TB_ASSERT_MSG(minimum <= maximum, "minimum <= maximum");
    /* The distance between the two, which may exceed INT64_MAX. Unsigned
     * arithmetic wraps by definition, so this is exact. */
    span = (uint64_t)maximum - (uint64_t)minimum;
    if (span == UINT64_MAX)
    {
        return tb_rng_to_signed(tb_rng_step(rng));
    }
    return tb_rng_to_signed((uint64_t)minimum + tb_rng_below(rng, span + 1));
}

float tb_rng_float(tb_rng *rng)
{
    tb_rng_check(rng);
    return (float)(tb_rng_step(rng) >> (64 - TB_RNG_FLOAT_BITS)) * TB_RNG_FLOAT_STEP;
}

double tb_rng_double(tb_rng *rng)
{
    tb_rng_check(rng);
    return (double)(tb_rng_step(rng) >> (64 - TB_RNG_DOUBLE_BITS)) * TB_RNG_DOUBLE_STEP;
}

float tb_rng_float_between(tb_rng *rng, float minimum, float maximum)
{
    const float width = maximum - minimum;
    float offset = 0.0f;

    tb_rng_check(rng);
    TB_ASSERT_MSG(isfinite(minimum) && isfinite(maximum) && isfinite(width),
                  "minimum, maximum, and their difference are finite");
    TB_ASSERT_MSG(minimum <= maximum, "minimum <= maximum");
    /* Two statements, not one expression: a compiler may fuse a multiply and
     * an add within one expression into one instruction that rounds once,
     * which Clang does by default where the processor has it, as on Apple
     * Silicon. Rounding twice everywhere keeps the result the same on every
     * platform. */
    offset = width * tb_rng_float(rng);
    return minimum + offset;
}

void tb_rng_jump(tb_rng *rng)
{
    uint64_t jumped[4] = {0};

    tb_rng_check(rng);
    for (size_t word = 0; word < 4; word++)
    {
        for (unsigned bit = 0; bit < 64; bit++)
        {
            if ((tb_rng_jump_polynomial[word] >> bit) & 1u)
            {
                jumped[0] ^= rng->state[0];
                jumped[1] ^= rng->state[1];
                jumped[2] ^= rng->state[2];
                jumped[3] ^= rng->state[3];
            }
            (void)tb_rng_step(rng);
        }
    }
    for (size_t word = 0; word < 4; word++)
    {
        rng->state[word] = jumped[word];
    }
}

tb_rng tb_rng_split(tb_rng *rng)
{
    tb_rng child = {0};

    tb_rng_check(rng);
    child = *rng;
    tb_rng_jump(rng);
    return child;
}
