/** @file rng.h
 * @brief Random numbers that can be repeated: the same seed gives the same
 * numbers, on every platform.
 *
 * A generator is a small value that the caller owns. Seeded the same way, it
 * gives the same sequence of numbers on Windows, Linux, and macOS, in Debug
 * and Release, from one run to the next. That is what a fixed-tick
 * simulation, a replay, procedural content, and a randomized test that must
 * be repeated from its seed all need.
 *
 * @code
 * tb_rng rng = {0};
 * tb_rng_seed(&rng, level_seed);
 *
 * const uint64_t loot = tb_rng_below(&rng, loot_table_count);
 * const float spread = tb_rng_float_between(&rng, -0.5f, 0.5f);
 * @endcode
 *
 * The generator is xoshiro256** (Blackman and Vigna, 2018): 256 bits of
 * state, a period of 2^256 - 1, a few nanoseconds a number, and output that
 * passes the standard statistical batteries. A seed is spread over the state
 * with SplitMix64, so that any 64-bit seed, 0 and 1 included, starts a good
 * sequence. Every result is defined bit for bit by these algorithms; the
 * tests check them against the published reference outputs.
 *
 * **Not for secrets.** Anyone who sees a few outputs can predict the rest.
 * Keys, nonces, and anything an adversary must not guess come from
 * tb_random_bytes() in badger/random.h.
 *
 * **Saving and restoring.** The generator is plain data: copy a tb_rng to
 * save its place in the sequence, and assign it back to return there.
 *
 * **Parallel work.** Give each job or thread a generator of its own, made with
 * tb_rng_split() from one seeded generator in a fixed order. The streams do
 * not overlap, and what each job draws does not depend on which thread runs it
 * or when.
 *
 * @par Threading
 * Externally synchronized: a generator is used by one thread at a time.
 * Different generators are independent.
 */
#ifndef TB_RNG_H
#define TB_RNG_H

#include "badger/api.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief A random number generator: xoshiro256** state.
 *
 * Seed it with tb_rng_seed() or tb_rng_seed_from_system() before use. The
 * all-zero state is not valid, because xoshiro would give zero forever; the
 * functions that draw a number assert against it.
 */
typedef struct tb_rng
{
    uint64_t state[4]; /**< The generator's state; set only through the functions here. */
} tb_rng;

/**
 * @brief Seed a generator.
 *
 * The same seed always starts the same sequence. Seeds that differ in a single
 * bit start sequences that look unrelated.
 *
 * @param rng The generator; required non-null.
 * @param seed Any value.
 */
TB_API void tb_rng_seed(tb_rng *rng, uint64_t seed);

/**
 * @brief Seed a generator from the operating system, and report the seed.
 *
 * For a run that should differ each time yet can still be repeated: record
 * the seed, in a log or a replay, and pass it to tb_rng_seed() to repeat it.
 *
 * @param rng The generator; required non-null.
 * @param seed Output, required non-null: the seed that was used.
 * @return Whether the system supplied a seed (see tb_random_bytes()). On
 * false, neither `rng` nor `seed` is changed.
 */
TB_API bool tb_rng_seed_from_system(tb_rng *rng, uint64_t *seed);

/**
 * @brief The next 64 random bits.
 * @param rng A seeded generator; required non-null.
 */
TB_API uint64_t tb_rng_next(tb_rng *rng);

/**
 * @brief The next 32 random bits: the upper half of tb_rng_next().
 * @param rng A seeded generator; required non-null.
 */
TB_API uint32_t tb_rng_next_u32(tb_rng *rng);

/**
 * @brief A random integer in `[0, bound)`, every value equally likely.
 *
 * Lemire's method: a multiplication instead of a division, and rarely a
 * redraw, so that no value is more likely than another, which taking the
 * remainder of a random number would make it. A bound that fits in 32 bits
 * draws 32 bits at a time, which is the faster path.
 *
 * @param rng A seeded generator; required non-null.
 * @param bound Exclusive upper limit; at least 1.
 */
TB_API uint64_t tb_rng_below(tb_rng *rng, uint64_t bound);

/**
 * @brief A random integer in `[minimum, maximum]`, both ends included, every
 * value equally likely.
 * @param rng A seeded generator; required non-null.
 * @param minimum Smallest value.
 * @param maximum Largest value; not less than `minimum`. The whole range of
 * int64_t is allowed.
 */
TB_API int64_t tb_rng_between(tb_rng *rng, int64_t minimum, int64_t maximum);

/**
 * @brief A random float in `[0, 1)`: one of the 2^24 multiples of 2^-24 below
 * 1, every one equally likely.
 * @param rng A seeded generator; required non-null.
 */
TB_API float tb_rng_float(tb_rng *rng);

/**
 * @brief A random double in `[0, 1)`: one of the 2^53 multiples of 2^-53
 * below 1, every one equally likely.
 * @param rng A seeded generator; required non-null.
 */
TB_API double tb_rng_double(tb_rng *rng);

/**
 * @brief A random float between two values.
 *
 * `minimum + (maximum - minimum) * tb_rng_float(rng)`. Rounding can give
 * `maximum` itself, so the result is in `[minimum, maximum]`.
 *
 * @param rng A seeded generator; required non-null.
 * @param minimum Smallest value; finite.
 * @param maximum Largest value; finite, not less than `minimum`, and such that
 * `maximum - minimum` is finite.
 */
TB_API float tb_rng_float_between(tb_rng *rng, float minimum, float maximum);

/**
 * @brief Advance a generator by 2^128 numbers at once.
 *
 * Equivalent to 2^128 calls of tb_rng_next(), in the time of a few hundred.
 * Each jump starts a stretch of the sequence that no amount of drawing from
 * the previous one would reach.
 *
 * @param rng A seeded generator; required non-null.
 */
TB_API void tb_rng_jump(tb_rng *rng);

/**
 * @brief Make a generator for parallel work, whose numbers do not overlap
 * with the parent's or with those of other splits.
 *
 * The child continues from where the parent is, and the parent jumps 2^128
 * ahead. Split in a fixed order, the same seed gives every child the same
 * numbers, whichever thread uses it.
 *
 * @param rng The parent, a seeded generator; required non-null.
 * @return The child.
 */
TB_API tb_rng tb_rng_split(tb_rng *rng);

#endif
