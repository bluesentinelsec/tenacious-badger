# Random numbers

The engine has two sources of randomness, for opposite purposes.

| Need | Use | Header |
| --- | --- | --- |
| Numbers that must repeat from a seed: simulation, replays, procedural content, gameplay, randomized tests | A generator, `tb_rng` | `rng.h` |
| Bytes no one may guess or repeat: keys, nonces, salts | `tb_random_bytes` | [`random.h`](platforms.md#random-bytes) |

The C library's `rand`, `srand`, and their POSIX relatives are neither
repeatable across platforms nor fit for secrets, and
`tools/check_foundation.py` rejects them.

## Generators

```c
tb_rng rng = {0};
tb_rng_seed(&rng, level_seed);

const uint64_t loot = tb_rng_below(&rng, loot_table_count);  /* 0 .. count - 1 */
const int64_t damage = tb_rng_between(&rng, 8, 12);          /* 8 .. 12 */
const float spread = tb_rng_float_between(&rng, -0.5f, 0.5f);
```

| Need | Use |
| --- | --- |
| Start a sequence | `tb_rng_seed(&rng, seed)`; `tb_rng_seed_from_system(&rng, &seed)` to start a new one and record its seed |
| Raw bits | `tb_rng_next` (64 bits), `tb_rng_next_u32` (32 bits) |
| An integer in a range | `tb_rng_below(&rng, bound)` for `[0, bound)`, `tb_rng_between(&rng, minimum, maximum)` with both ends included |
| A fraction | `tb_rng_float` and `tb_rng_double` in `[0, 1)`, `tb_rng_float_between(&rng, minimum, maximum)` |
| Streams for parallel work | `tb_rng_split(&rng)`, `tb_rng_jump(&rng)` |

**The same seed gives the same numbers everywhere.** The generator is
xoshiro256** (Blackman and Vigna), seeded through SplitMix64 so that every
64-bit seed starts a good sequence. Every function's result is defined bit
for bit, the same on Windows, Linux, and macOS, in Debug and Release. The
tests check the raw outputs, the seeding, and the jump against the reference
code's published outputs, and the functions built on them against one digest
that every platform in CI must reproduce.

**Integers are unbiased.** `tb_rng_below` uses Lemire's method: the number is
scaled by multiplication, and the few draws that would make some values more
likely than others are redrawn. `draw % bound`, the usual shortcut, favors
small values. Bounds that fit in 32 bits take a faster path that draws 32
bits at a time.

**Floats are evenly spaced.** `tb_rng_float` returns one of the 2^24
multiples of 2^-24 below 1, and `tb_rng_double` one of the 2^53 multiples of
2^-53, each equally likely. Neither returns 1. `tb_rng_float_between` can
return its maximum through rounding.

**A generator is plain data.** It is 32 bytes the caller owns: no global
state, no allocation. Copy it to save the place in the sequence, as a replay
or a save game would, and assign it back to return there. The all-zero state
is not valid; seed a generator before drawing from it, which the functions
assert.

**One generator per thread or stream.** A generator is externally
synchronized. For parallel work, split one seeded generator into a child for
each job, in a fixed order: each child gets a stretch of 2^128 numbers that no
other reaches, and what a job draws does not depend on which thread runs it.

```c
tb_rng frame_rng = {0};
tb_rng_seed(&frame_rng, simulation_seed ^ frame_number);
for (size_t index = 0; index < job_count; index++)
{
    jobs[index].rng = tb_rng_split(&frame_rng);
}
```

**Not for secrets.** A few outputs reveal the state and so every later one.
Anything an adversary must not predict comes from `tb_random_bytes`.

`tb_bench rng` measures the generator.
