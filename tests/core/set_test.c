/* A set answers one question, whether something is a member, and the set
 * operations are defined by that question alone. The cases therefore check
 * every set against a model that answers it directly: a 64-bit mask for sets
 * drawn from sixty-four possible members, where each operation is one machine
 * instruction that cannot be wrong. Larger sets are checked by the laws that
 * the operations must obey whatever their members are. */
#include "badger/set.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

enum
{
    /* The possible members of a set that is checked against a mask. */
    UNIVERSE_SIZE = 64
};

/* An integer as a member. Zero is the NULL pointer, which is a member like
 * any other. */
static const void *member_of(size_t number)
{
    return (const void *)(uintptr_t)number;
}

/* --- hash functions that collide on purpose ----------------------------------- */

static bool same_address(const void *left, const void *right)
{
    return left == right;
}

static uint64_t hash_to_zero(const void *member)
{
    (void)member;
    return 0;
}

static uint64_t hash_to_few_slots(const void *member)
{
    return ((uint64_t)(uintptr_t)member % 5) * UINT64_C(0x3333333333333333) + UINT64_MAX - 2;
}

typedef struct hashing
{
    const char *name;
    tb_set_hash_function *hash;
    tb_set_equal_function *equal;
} hashing;

static const hashing hashings[] = {
    {"by address", NULL, NULL},
    {"every member in one slot", hash_to_zero, same_address},
    {"members in five slots", hash_to_few_slots, same_address},
};

/* --- sets and masks ----------------------------------------------------------- */

static size_t count_bits(uint64_t mask)
{
    size_t count = {0};

    for (size_t number = 0; number < UNIVERSE_SIZE; number++)
    {
        count += (size_t)((mask >> number) & 1);
    }
    return count;
}

/* The set whose members are the numbers of the bits set in a mask. */
static tb_set *set_of(const hashing *how, uint64_t mask)
{
    tb_set *const set = tb_set_create(NULL, 0, how->hash, how->equal);

    for (size_t number = 0; number < UNIVERSE_SIZE; number++)
    {
        if (((mask >> number) & 1) != 0)
        {
            (void)tb_set_put(set, member_of(number));
        }
    }
    return set;
}

/* Checks that a set has exactly the members a mask names, by every way of
 * asking it: its length, the membership test, and a walk over its members. */
static void expect_set(const tb_set *set, uint64_t mask)
{
    const void *member = NULL;
    uint64_t walked = {0};
    size_t cursor = {0};
    size_t wrong = {0};

    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, count_bits(mask));
    for (size_t number = 0; number < UNIVERSE_SIZE; number++)
    {
        if (tb_set_contains(set, member_of(number)) != (((mask >> number) & 1) != 0))
        {
            wrong++;
        }
    }
    while (tb_set_next(set, &cursor, &member))
    {
        const size_t number = (size_t)(uintptr_t)member;

        /* A member outside the universe, or one returned twice. */
        if (number >= UNIVERSE_SIZE || ((walked >> number) & 1) != 0)
        {
            wrong++;
            continue;
        }
        walked |= UINT64_C(1) << number;
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    TB_TEST_EXPECT_HEX(walked, ==, mask);
}

/* A mask with few bits, many bits, or about half, so that empty and full
 * sets and every size between them come up. */
static uint64_t random_mask(void)
{
    switch (tb_test_random_below(5))
    {
    case 0:
        return 0;
    case 1:
        return UINT64_MAX;
    case 2:
        return tb_test_random_u64() & tb_test_random_u64() & tb_test_random_u64();
    case 3:
        return tb_test_random_u64() | tb_test_random_u64() | tb_test_random_u64();
    default:
        return tb_test_random_u64();
    }
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_set, a_new_set_is_empty)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_set *const set = tb_set_create(&allocator, 0, NULL, NULL);
    tb_set *const sized = tb_set_create(&allocator, 1000, NULL, NULL);
    size_t requests = {0};

    TB_TEST_REQUIRE_POINTER(set, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, 0);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 0);
    TB_TEST_EXPECT(!tb_set_contains(set, member_of(1)));
    TB_TEST_EXPECT(!tb_set_contains(set, NULL));
    TB_TEST_EXPECT(!tb_set_remove(set, member_of(1), NULL));
    TB_TEST_EXPECT_UINT(tb_set_to_array(set, NULL, 0), ==, 0);
    tb_set_clear(set);
    expect_set(set, 0);

    /* A set told what to expect holds that much without growing. */
    TB_TEST_EXPECT_UINT(tb_set_capacity(sized), >=, 1000);
    requests = tracker.request_count;
    for (size_t number = 0; number < 1000; number++)
    {
        (void)tb_set_put(sized, member_of(number));
    }
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    TB_TEST_EXPECT_UINT(tb_set_length(sized), ==, 1000);

    tb_set_destroy(sized);
    tb_set_destroy(set);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_set, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_set_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- members ------------------------------------------------------------------ */

TB_TEST(core_set, put_adds_a_member_once_and_remove_takes_it_out)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_set *const set = tb_set_create(NULL, 0, hashings[row].hash, hashings[row].equal);
        const void *stored = member_of(99);

        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        TB_TEST_EXPECT(tb_set_put(set, member_of(7)));
        TB_TEST_EXPECT(tb_set_put(set, member_of(8)));
        expect_set(set, (UINT64_C(1) << 7) | (UINT64_C(1) << 8));

        /* A member that is there is not added again. */
        TB_TEST_EXPECT(!tb_set_put(set, member_of(7)));
        TB_TEST_EXPECT(!tb_set_put(set, member_of(7)));
        TB_TEST_EXPECT_UINT(tb_set_length(set), ==, 2);

        /* NULL, the integer zero, is a member like any other. */
        TB_TEST_EXPECT(!tb_set_contains(set, NULL));
        TB_TEST_EXPECT(tb_set_put(set, NULL));
        TB_TEST_EXPECT(!tb_set_put(set, NULL));
        expect_set(set, (UINT64_C(1) << 7) | (UINT64_C(1) << 8) | 1);

        TB_TEST_EXPECT(tb_set_remove(set, member_of(7), &stored));
        TB_TEST_EXPECT_POINTER(stored, ==, member_of(7));
        /* A member that is not there is not removed, and the pointer that
         * would receive it is left alone. */
        stored = member_of(99);
        TB_TEST_EXPECT(!tb_set_remove(set, member_of(7), &stored));
        TB_TEST_EXPECT(!tb_set_remove(set, member_of(50), NULL));
        TB_TEST_EXPECT_POINTER(stored, ==, member_of(99));
        TB_TEST_EXPECT(tb_set_remove(set, NULL, NULL));
        expect_set(set, UINT64_C(1) << 8);

        tb_set_clear(set);
        expect_set(set, 0);
        TB_TEST_EXPECT(tb_set_put(set, member_of(8)));
        tb_set_destroy(set);
    }
}

TB_TEST(core_set, string_members_are_compared_by_content_and_the_first_one_is_kept)
{
    static const char *const words[] = {"", "a", "ab", "badger", "badgers", "Badger"};
    tb_set *const set = tb_set_create(NULL, 0, tb_set_string_hash, tb_set_string_equal);
    char first_copy[] = "badger";
    char second_copy[] = "badger";
    const void *stored = NULL;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(words); index++)
    {
        TB_TEST_EXPECT(tb_set_put(set, words[index]));
    }
    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, TB_TEST_COUNT_OF(words));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(words); index++)
    {
        TB_TEST_CONTEXT("\"%s\"", words[index]);
        TB_TEST_EXPECT(tb_set_contains(set, words[index]));
    }
    TB_TEST_EXPECT(!tb_set_contains(set, "badge"));
    TB_TEST_EXPECT(!tb_set_contains(set, "BADGER"));

    /* An equal string at another address is the same member: it is found,
     * it is not added, and the set goes on holding the first pointer. */
    TB_TEST_EXPECT(tb_set_contains(set, first_copy));
    TB_TEST_EXPECT(!tb_set_put(set, second_copy));
    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, TB_TEST_COUNT_OF(words));
    TB_TEST_EXPECT(tb_set_find(set, first_copy, &stored));
    TB_TEST_EXPECT_POINTER(stored, ==, words[3]);
    TB_TEST_EXPECT(tb_set_find(set, first_copy, NULL));
    /* A miss leaves the pointer it was given alone. */
    TB_TEST_EXPECT(!tb_set_find(set, "missing", &stored));
    TB_TEST_EXPECT_POINTER(stored, ==, words[3]);

    stored = NULL;
    TB_TEST_EXPECT(tb_set_remove(set, second_copy, &stored));
    TB_TEST_EXPECT_POINTER(stored, ==, words[3]);
    TB_TEST_EXPECT(!tb_set_contains(set, "badger"));
    TB_TEST_EXPECT_HEX(tb_set_string_hash("badger"), ==, tb_hash_string("badger"));
    TB_TEST_EXPECT(tb_set_string_equal(first_copy, second_copy));
    TB_TEST_EXPECT(!tb_set_string_equal(first_copy, "badgers"));
    tb_set_destroy(set);
}

TB_TEST(core_set, a_set_holds_a_million_members)
{
    enum
    {
        MEMBER_COUNT = 1000000
    };
    tb_set *const set = tb_set_create(NULL, 0, NULL, NULL);
    size_t wrong = {0};

    for (size_t number = 0; number < MEMBER_COUNT; number++)
    {
        if (!tb_set_put(set, member_of(number * 16)))
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, MEMBER_COUNT);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), >=, MEMBER_COUNT);
    for (size_t number = 0; number < MEMBER_COUNT; number++)
    {
        if (!tb_set_contains(set, member_of(number * 16)) ||
            tb_set_contains(set, member_of(number * 16 + 1)) ||
            tb_set_put(set, member_of(number * 16)))
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    TB_TEST_EXPECT_UINT(tb_set_length(set), ==, MEMBER_COUNT);
    tb_set_destroy(set);
}

TB_TEST(core_set, a_set_grows_when_it_is_full_and_shrinks_when_asked)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_set *const set = tb_set_create(&allocator, 0, NULL, NULL);
    size_t requests = {0};

    for (size_t number = 0; number < 12; number++)
    {
        (void)tb_set_put(set, member_of(number));
    }
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 12);
    (void)tb_set_put(set, member_of(12));
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 24);
    expect_set(set, (UINT64_C(1) << 13) - 1);

    tb_set_reserve(set, 100);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), >=, 100);
    /* Room that is already there is not asked for again. */
    requests = tracker.request_count;
    tb_set_reserve(set, 50);
    TB_TEST_EXPECT(tb_set_try_reserve(set, 100));
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    expect_set(set, (UINT64_C(1) << 13) - 1);

    tb_set_shrink_to_fit(set);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 24);
    expect_set(set, (UINT64_C(1) << 13) - 1);
    tb_set_clear(set);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 24);
    tb_set_shrink_to_fit(set);
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 0);
    /* The set, and its table. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 2);
    tb_set_destroy(set);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- all members -------------------------------------------------------------- */

typedef struct visit_record
{
    uint64_t seen;
    size_t call_count;
    size_t repeat_count;
} visit_record;

static void record_member(const void *member, void *context)
{
    visit_record *const record = context;
    const uint64_t bit = UINT64_C(1) << ((uintptr_t)member % UNIVERSE_SIZE);

    record->call_count++;
    if ((record->seen & bit) != 0)
    {
        record->repeat_count++;
    }
    record->seen |= bit;
}

TB_TEST(core_set, visit_next_and_to_array_reach_every_member_once)
{
    static const size_t capacities[] = {0, 1, 5, 17, 18, 19, 64};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        /* Eighteen members, spread over the universe. */
        const uint64_t mask = UINT64_C(0x8421084210842109) | (UINT64_C(0x1F) << 40);
        const size_t member_count = count_bits(mask);
        tb_set *const set = set_of(&hashings[row], mask);
        tb_set *const empty = set_of(&hashings[row], 0);
        visit_record record = {0};
        const void *member = member_of(77);
        size_t cursor = {0};

        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        TB_TEST_REQUIRE_UINT(member_count, ==, 18);
        tb_set_visit(empty, record_member, &record);
        TB_TEST_EXPECT_UINT(record.call_count, ==, 0);
        /* An empty set has no next member, and leaves the pointer alone. */
        TB_TEST_EXPECT(!tb_set_next(empty, &cursor, &member));
        TB_TEST_EXPECT_POINTER(member, ==, member_of(77));

        tb_set_visit(set, record_member, &record);
        TB_TEST_EXPECT_UINT(record.call_count, ==, member_count);
        TB_TEST_EXPECT_UINT(record.repeat_count, ==, 0);
        TB_TEST_EXPECT_HEX(record.seen, ==, mask);
        /* A visit that has finished leaves the set free to change. */
        TB_TEST_EXPECT(!tb_set_put(set, member_of(0)));

        for (size_t index = 0; index < TB_TEST_COUNT_OF(capacities); index++)
        {
            const void *members[UNIVERSE_SIZE + 1] = {0};
            const size_t capacity = capacities[index];
            const size_t copied = capacity < member_count ? capacity : member_count;
            uint64_t copied_mask = {0};

            TB_TEST_CONTEXT("hashing %s, capacity %zu", hashings[row].name, capacity);
            for (size_t slot = 0; slot <= UNIVERSE_SIZE; slot++)
            {
                members[slot] = member_of(1000);
            }
            TB_TEST_EXPECT_UINT(tb_set_to_array(set, members, capacity), ==, member_count);
            for (size_t slot = 0; slot < copied; slot++)
            {
                const size_t number = (size_t)(uintptr_t)members[slot];

                TB_TEST_REQUIRE_UINT(number, <, UNIVERSE_SIZE);
                /* Each member is copied at most once. */
                TB_TEST_EXPECT((copied_mask & (UINT64_C(1) << number)) == 0);
                copied_mask |= UINT64_C(1) << number;
            }
            TB_TEST_EXPECT_HEX(copied_mask & ~mask, ==, 0);
            TB_TEST_EXPECT_UINT(count_bits(copied_mask), ==, copied);
            /* Nothing is written beyond the capacity or beyond the members. */
            TB_TEST_EXPECT_POINTER(members[copied], ==, member_of(1000));
        }
        tb_set_destroy(empty);
        tb_set_destroy(set);
    }
}

/* --- copies ------------------------------------------------------------------- */

TB_TEST(core_set, a_copy_has_the_same_members_and_a_life_of_its_own)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const uint64_t mask = UINT64_C(0x00FF00FF0000FFFF);

    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_set *const set = set_of(&hashings[row], mask);
        tb_set *const copy = tb_set_copy(set, &allocator);
        tb_set *const second_copy = tb_set_copy(copy, NULL);

        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        expect_set(copy, mask);
        TB_TEST_EXPECT(tb_set_equal(set, copy));
        /* The copy is in the allocator it was given: the set and its table,
         * and the slots of the table. */
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 3);

        /* Changing either leaves the other as it was. */
        (void)tb_set_remove(copy, member_of(0), NULL);
        (void)tb_set_put(copy, member_of(63));
        expect_set(set, mask);
        tb_set_clear(set);
        expect_set(copy, (mask & ~UINT64_C(1)) | (UINT64_C(1) << 63));
        expect_set(second_copy, mask);

        tb_set_destroy(second_copy);
        tb_set_destroy(copy);
        tb_set_destroy(set);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    }
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_set, a_copy_compares_members_as_the_original_does)
{
    tb_set *const set = tb_set_create(NULL, 0, tb_set_string_hash, tb_set_string_equal);
    tb_set *copy = NULL;
    tb_set *empty_copy = NULL;
    char same_content[] = "badger";

    empty_copy = tb_set_copy(set, NULL);
    (void)tb_set_put(set, "badger");
    copy = tb_set_copy(set, NULL);
    TB_TEST_EXPECT(tb_set_contains(copy, same_content));
    TB_TEST_EXPECT(!tb_set_put(copy, same_content));
    /* The copy of an empty set compares the same way once it has members. */
    TB_TEST_EXPECT_UINT(tb_set_length(empty_copy), ==, 0);
    TB_TEST_EXPECT(tb_set_put(empty_copy, "badger"));
    TB_TEST_EXPECT(tb_set_contains(empty_copy, same_content));
    TB_TEST_EXPECT(tb_set_equal(copy, empty_copy));
    tb_set_destroy(empty_copy);
    tb_set_destroy(copy);
    tb_set_destroy(set);
}

/* --- set operations against the mask model ------------------------------------ */

/* Every operation and comparison on one pair of sets, against the same
 * operation on their masks. `right` may be `left` itself. */
static void expect_operations(const tb_set *left, uint64_t left_mask, const tb_set *right,
                              uint64_t right_mask)
{
    tb_set *const both = tb_set_union(left, right, NULL);
    tb_set *const shared = tb_set_intersection(left, right, NULL);
    tb_set *const only_left = tb_set_difference(left, right, NULL);
    tb_set *const only_right = tb_set_difference(right, left, NULL);
    tb_set *const one_side = tb_set_symmetric_difference(left, right, NULL);

    expect_set(both, left_mask | right_mask);
    expect_set(shared, left_mask & right_mask);
    expect_set(only_left, left_mask & ~right_mask);
    expect_set(only_right, right_mask & ~left_mask);
    expect_set(one_side, left_mask ^ right_mask);
    /* The operands are as they were. */
    expect_set(left, left_mask);
    expect_set(right, right_mask);

    TB_TEST_EXPECT(tb_set_equal(left, right) == (left_mask == right_mask));
    TB_TEST_EXPECT(tb_set_is_subset(left, right) == ((left_mask & ~right_mask) == 0));
    TB_TEST_EXPECT(tb_set_is_subset(right, left) == ((right_mask & ~left_mask) == 0));
    TB_TEST_EXPECT(tb_set_is_proper_subset(left, right) ==
                   ((left_mask & ~right_mask) == 0 && left_mask != right_mask));
    TB_TEST_EXPECT(tb_set_is_proper_subset(right, left) ==
                   ((right_mask & ~left_mask) == 0 && left_mask != right_mask));

    tb_set_destroy(one_side);
    tb_set_destroy(only_right);
    tb_set_destroy(only_left);
    tb_set_destroy(shared);
    tb_set_destroy(both);
}

TB_TEST(core_set, operations_agree_with_bit_masks_on_chosen_sets)
{
    /* Empty, full, one member at either end, halves that are disjoint or
     * overlap, and a set inside another. */
    static const uint64_t masks[] = {
        0,
        UINT64_MAX,
        1,
        UINT64_C(0x8000000000000000),
        UINT64_C(0x00000000FFFFFFFF),
        UINT64_C(0xFFFFFFFF00000000),
        UINT64_C(0x0000FFFFFFFF0000),
        UINT64_C(0x0000000000FF0000),
        UINT64_C(0xAAAAAAAAAAAAAAAA),
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        for (size_t first = 0; first < TB_TEST_COUNT_OF(masks); first++)
        {
            tb_set *const left = set_of(&hashings[row], masks[first]);

            for (size_t second = 0; second < TB_TEST_COUNT_OF(masks); second++)
            {
                tb_set *const right = set_of(&hashings[row], masks[second]);

                TB_TEST_CONTEXT("hashing %s, masks %zu and %zu", hashings[row].name, first, second);
                expect_operations(left, masks[first], right, masks[second]);
                tb_set_destroy(right);
            }
            /* A set with itself. */
            TB_TEST_CONTEXT("hashing %s, mask %zu with itself", hashings[row].name, first);
            expect_operations(left, masks[first], left, masks[first]);
            tb_set_destroy(left);
        }
    }
}

TB_TEST(core_set, operations_agree_with_bit_masks_on_random_sets)
{
    enum
    {
        PAIR_COUNT = 400
    };

    for (size_t pair = 0; pair < PAIR_COUNT; pair++)
    {
        const hashing *const how = &hashings[pair % TB_TEST_COUNT_OF(hashings)];
        const uint64_t left_mask = random_mask();
        const uint64_t right_mask = random_mask();
        tb_set *const left = set_of(how, left_mask);
        tb_set *const right = set_of(how, right_mask);

        TB_TEST_CONTEXT("hashing %s, masks 0x%016llx and 0x%016llx", how->name,
                        (unsigned long long)left_mask, (unsigned long long)right_mask);
        expect_operations(left, left_mask, right, right_mask);
        tb_set_destroy(right);
        tb_set_destroy(left);
    }
}

/* --- the laws of set algebra on larger sets ----------------------------------- */

/* A set of about `count` members drawn from the numbers below `range`. */
static tb_set *random_set(size_t count, size_t range)
{
    tb_set *const set = tb_set_create(NULL, 0, NULL, NULL);

    for (size_t index = 0; index < count; index++)
    {
        (void)tb_set_put(set, member_of((size_t)tb_test_random_below(range)));
    }
    return set;
}

TB_TEST(core_set, operations_obey_the_laws_of_set_algebra)
{
    enum
    {
        ROUND_COUNT = 12,
        RANGE = 6000
    };

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        /* Sets of different sizes that overlap in part. */
        tb_set *const a = random_set(500 + round * 300, RANGE);
        tb_set *const b = random_set(2000, RANGE);
        tb_set *const c = random_set(round * 100, RANGE);
        tb_set *const a_or_b = tb_set_union(a, b, NULL);
        tb_set *const b_or_a = tb_set_union(b, a, NULL);
        tb_set *const a_and_b = tb_set_intersection(a, b, NULL);
        tb_set *const b_and_a = tb_set_intersection(b, a, NULL);
        tb_set *const b_or_c = tb_set_union(b, c, NULL);
        tb_set *const b_and_c = tb_set_intersection(b, c, NULL);
        tb_set *const left_union = tb_set_union(a_or_b, c, NULL);
        tb_set *const right_union = tb_set_union(a, b_or_c, NULL);
        tb_set *const left_intersection = tb_set_intersection(a_and_b, c, NULL);
        tb_set *const right_intersection = tb_set_intersection(a, b_and_c, NULL);
        tb_set *const a_or_a = tb_set_union(a, a, NULL);
        tb_set *const a_and_a = tb_set_intersection(a, a, NULL);
        tb_set *const a_less_a = tb_set_difference(a, a, NULL);
        tb_set *const a_less_b = tb_set_difference(a, b, NULL);
        tb_set *const b_less_a = tb_set_difference(b, a, NULL);
        tb_set *const a_xor_b = tb_set_symmetric_difference(a, b, NULL);
        tb_set *const b_xor_a = tb_set_symmetric_difference(b, a, NULL);
        tb_set *const both_differences = tb_set_union(a_less_b, b_less_a, NULL);
        tb_set *const a_again = tb_set_union(a_less_b, a_and_b, NULL);
        tb_set *const nothing = tb_set_intersection(a_less_b, a_and_b, NULL);

        TB_TEST_CONTEXT("round %zu: %zu, %zu, and %zu members", round, tb_set_length(a),
                        tb_set_length(b), tb_set_length(c));
        /* Commutative. */
        TB_TEST_EXPECT(tb_set_equal(a_or_b, b_or_a));
        TB_TEST_EXPECT(tb_set_equal(a_and_b, b_and_a));
        TB_TEST_EXPECT(tb_set_equal(a_xor_b, b_xor_a));
        /* Associative. */
        TB_TEST_EXPECT(tb_set_equal(left_union, right_union));
        TB_TEST_EXPECT(tb_set_equal(left_intersection, right_intersection));
        /* A set with itself. */
        TB_TEST_EXPECT(tb_set_equal(a_or_a, a));
        TB_TEST_EXPECT(tb_set_equal(a_and_a, a));
        TB_TEST_EXPECT_UINT(tb_set_length(a_less_a), ==, 0);
        /* The sizes of the union and the intersection add up to the sizes
         * of the operands. */
        TB_TEST_EXPECT_UINT(tb_set_length(a_or_b) + tb_set_length(a_and_b), ==,
                            tb_set_length(a) + tb_set_length(b));
        /* The difference and the intersection divide a set in two. */
        TB_TEST_EXPECT(tb_set_equal(a_again, a));
        TB_TEST_EXPECT_UINT(tb_set_length(nothing), ==, 0);
        TB_TEST_EXPECT_UINT(tb_set_length(a_less_b) + tb_set_length(a_and_b), ==, tb_set_length(a));
        /* The symmetric difference is what each has that the other lacks. */
        TB_TEST_EXPECT(tb_set_equal(a_xor_b, both_differences));
        /* Each result lies where it must. */
        TB_TEST_EXPECT(tb_set_is_subset(a, a_or_b));
        TB_TEST_EXPECT(tb_set_is_subset(b, a_or_b));
        TB_TEST_EXPECT(tb_set_is_subset(a_and_b, a));
        TB_TEST_EXPECT(tb_set_is_subset(a_and_b, b));
        TB_TEST_EXPECT(tb_set_is_subset(a_less_b, a));
        TB_TEST_REQUIRE_UINT(tb_set_length(a_and_b), >, 0);
        TB_TEST_REQUIRE_UINT(tb_set_length(a_less_b), >, 0);
        TB_TEST_EXPECT(tb_set_is_proper_subset(a_less_b, a));
        TB_TEST_EXPECT(!tb_set_is_subset(a, a_less_b));
        TB_TEST_EXPECT(!tb_set_equal(a, a_less_b));

        tb_set_destroy(nothing);
        tb_set_destroy(a_again);
        tb_set_destroy(both_differences);
        tb_set_destroy(b_xor_a);
        tb_set_destroy(a_xor_b);
        tb_set_destroy(b_less_a);
        tb_set_destroy(a_less_b);
        tb_set_destroy(a_less_a);
        tb_set_destroy(a_and_a);
        tb_set_destroy(a_or_a);
        tb_set_destroy(right_intersection);
        tb_set_destroy(left_intersection);
        tb_set_destroy(right_union);
        tb_set_destroy(left_union);
        tb_set_destroy(b_and_c);
        tb_set_destroy(b_or_c);
        tb_set_destroy(b_and_a);
        tb_set_destroy(a_and_b);
        tb_set_destroy(b_or_a);
        tb_set_destroy(a_or_b);
        tb_set_destroy(c);
        tb_set_destroy(b);
        tb_set_destroy(a);
    }
}

TB_TEST(core_set, sets_of_equal_size_with_different_members_are_not_equal)
{
    tb_set *const left = set_of(&hashings[0], UINT64_C(0x0F));
    tb_set *const right = set_of(&hashings[0], UINT64_C(0x17));

    /* Four members each, three of them shared: neither holds the other. */
    TB_TEST_EXPECT(!tb_set_equal(left, right));
    TB_TEST_EXPECT(!tb_set_is_subset(left, right));
    TB_TEST_EXPECT(!tb_set_is_subset(right, left));
    TB_TEST_EXPECT(!tb_set_is_proper_subset(left, right));
    /* A set is a subset of itself, and not a proper one. */
    TB_TEST_EXPECT(tb_set_equal(left, left));
    TB_TEST_EXPECT(tb_set_is_subset(left, left));
    TB_TEST_EXPECT(!tb_set_is_proper_subset(left, left));
    tb_set_destroy(right);
    tb_set_destroy(left);
}

/* --- results ------------------------------------------------------------------ */

typedef tb_set *set_operation(const tb_set *left, const tb_set *right,
                              const tb_allocator *allocator);

typedef struct operation_row
{
    const char *name;
    set_operation *operation;
    /* What the operation makes of the masks 0x0F and 0x3C. */
    uint64_t expected;
} operation_row;

/* The operations as functions of this file: the address of a function in a
 * shared library is not a constant that a table can be initialized with. */
static tb_set *union_of(const tb_set *left, const tb_set *right, const tb_allocator *allocator)
{
    return tb_set_union(left, right, allocator);
}

static tb_set *intersection_of(const tb_set *left, const tb_set *right,
                               const tb_allocator *allocator)
{
    return tb_set_intersection(left, right, allocator);
}

static tb_set *difference_of(const tb_set *left, const tb_set *right, const tb_allocator *allocator)
{
    return tb_set_difference(left, right, allocator);
}

static tb_set *symmetric_difference_of(const tb_set *left, const tb_set *right,
                                       const tb_allocator *allocator)
{
    return tb_set_symmetric_difference(left, right, allocator);
}

static const operation_row operations[] = {
    {"union", union_of, UINT64_C(0x3F)},
    {"intersection", intersection_of, UINT64_C(0x0C)},
    {"difference", difference_of, UINT64_C(0x03)},
    {"symmetric difference", symmetric_difference_of, UINT64_C(0x33)},
};

TB_TEST(core_set, a_result_is_a_set_of_its_own_in_the_allocator_it_was_given)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(operations); row++)
    {
        tb_test_allocator tracker = {0};
        const tb_allocator allocator = tb_test_allocator_start(&tracker);
        tb_set *const left = set_of(&hashings[0], UINT64_C(0x0F));
        tb_set *const right = set_of(&hashings[0], UINT64_C(0x3C));
        tb_set *const result = operations[row].operation(left, right, &allocator);

        TB_TEST_CONTEXT("%s", operations[row].name);
        expect_set(result, operations[row].expected);
        /* All of the result is in the allocator it was given, and not in
         * the heap where its operands are: the set, its table, and the
         * slots of the table. */
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 3);

        /* Changing the operands does not change the result. */
        tb_set_clear(left);
        (void)tb_set_put(right, member_of(60));
        expect_set(result, operations[row].expected);
        /* Changing the result does not change the operands. */
        (void)tb_set_put(result, member_of(50));
        tb_set_clear(result);
        expect_set(left, 0);
        expect_set(right, UINT64_C(0x3C) | (UINT64_C(1) << 60));

        tb_set_destroy(result);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
        TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
        tb_set_destroy(right);
        tb_set_destroy(left);
    }
}

TB_TEST(core_set, a_result_compares_members_as_its_operands_do)
{
    char left_badger[] = "badger";
    char right_badger[] = "badger";
    char left_only[] = "sett";
    char right_only[] = "burrow";
    char other_badger[] = "badger";
    char new_member[] = "new";
    /* For the union, the intersection, the difference, and the symmetric
     * difference, in the order of `operations`. */
    static const size_t expected_lengths[] = {3, 1, 1, 2};
    static const bool has_the_shared_member[] = {true, true, false, false};
    static const bool has_the_left_member[] = {true, false, true, true};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(operations); row++)
    {
        tb_set *const left = tb_set_create(NULL, 0, tb_set_string_hash, tb_set_string_equal);
        tb_set *const right = tb_set_create(NULL, 0, tb_set_string_hash, tb_set_string_equal);
        tb_set *result = NULL;
        const void *stored = NULL;

        (void)tb_set_put(left, left_badger);
        (void)tb_set_put(left, left_only);
        (void)tb_set_put(right, right_badger);
        (void)tb_set_put(right, right_only);
        result = operations[row].operation(left, right, NULL);
        TB_TEST_CONTEXT("%s", operations[row].name);
        /* Equal strings in the two operands are one member: the union has
         * three members and not four, and the intersection has one. */
        TB_TEST_EXPECT_UINT(tb_set_length(result), ==, expected_lengths[row]);
        TB_TEST_EXPECT(tb_set_contains(result, other_badger) == has_the_shared_member[row]);
        TB_TEST_EXPECT(tb_set_contains(result, "sett") == has_the_left_member[row]);
        /* A member the operands share is in the result by the pointer of
         * the left operand. */
        if (tb_set_find(result, other_badger, &stored))
        {
            TB_TEST_EXPECT_POINTER(stored, ==, left_badger);
        }
        /* The result goes on comparing by content. */
        TB_TEST_EXPECT(tb_set_put(result, new_member));
        TB_TEST_EXPECT(tb_set_contains(result, "new"));
        TB_TEST_EXPECT(!tb_set_put(result, "new"));
        tb_set_destroy(result);
        tb_set_destroy(right);
        tb_set_destroy(left);
    }
}

/* --- running out of memory ---------------------------------------------------- */

static void put_a_new_member(void *context)
{
    (void)tb_set_put(context, member_of(40));
}

static void reserve_a_million(void *context)
{
    tb_set_reserve(context, 1000000);
}

static void create_a_set(void *context)
{
    (void)tb_set_create(context, 0, NULL, NULL);
}

typedef struct failing_operation
{
    const tb_set *left;
    const tb_set *right;
    const tb_allocator *allocator;
    set_operation *operation;
} failing_operation;

static void run_operation(void *context)
{
    const failing_operation *const call = context;

    (void)call->operation(call->left, call->right, call->allocator);
}

static void copy_with_the_allocator(void *context)
{
    const failing_operation *const call = context;

    (void)tb_set_copy(call->left, call->allocator);
}

TB_TEST(core_set, an_exhausted_allocator_ends_growth_and_leaves_the_set_intact)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_set *const set = tb_set_create(&allocator, 0, NULL, NULL);
    tb_set *const other = set_of(&hashings[0], UINT64_C(0xFF0));
    failing_operation call = {.left = set, .right = other, .allocator = &allocator};
    size_t live_blocks = {0};

    /* Filled to its capacity, so that one more member must grow it. */
    for (size_t number = 0; number < 12; number++)
    {
        (void)tb_set_put(set, member_of(number));
    }
    TB_TEST_REQUIRE_UINT(tb_set_capacity(set), ==, 12);
    live_blocks = tracker.live_block_count;
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(put_a_new_member, set, "out of memory");
    TB_TEST_EXPECT_TRAP(reserve_a_million, set, "out of memory");
    TB_TEST_EXPECT_TRAP(create_a_set, &allocator, "out of memory");
    TB_TEST_EXPECT_TRAP(copy_with_the_allocator, &call, "out of memory");
    /* A set whose table was made, and which then cannot be made itself,
     * does not leave the table behind. */
    tb_test_allocator_fail_after(&tracker, 1);
    TB_TEST_EXPECT_TRAP(create_a_set, &allocator, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, live_blocks);
    tb_test_allocator_fail_after(&tracker, 0);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(operations); row++)
    {
        call.operation = operations[row].operation;
        TB_TEST_CONTEXT("%s", operations[row].name);
        TB_TEST_EXPECT_TRAP(run_operation, &call, "out of memory");
    }
    TB_TEST_EXPECT(!tb_set_try_reserve(set, 13));
    TB_TEST_EXPECT(!tb_set_try_reserve(set, SIZE_MAX));
    /* A member that is there needs no memory to be put again. */
    TB_TEST_EXPECT(!tb_set_put(set, member_of(3)));
    tb_test_allocator_stop_failing(&tracker);

    expect_set(set, UINT64_C(0xFFF));
    expect_set(other, UINT64_C(0xFF0));
    TB_TEST_EXPECT_UINT(tb_set_capacity(set), ==, 12);
    TB_TEST_EXPECT(tb_set_put(set, member_of(40)));
    expect_set(set, UINT64_C(0xFFF) | (UINT64_C(1) << 40));
    tb_set_destroy(other);
    tb_set_destroy(set);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_set, sets_and_their_results_can_live_in_an_arena)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    const size_t heap_blocks = tb_memory_allocation_count();
    tb_set *const left = tb_set_create(&allocator, 0, NULL, NULL);
    tb_set *const right = tb_set_create(&allocator, 0, NULL, NULL);
    tb_set *both = NULL;

    for (size_t number = 0; number < 40; number++)
    {
        (void)tb_set_put(left, member_of(number));
        (void)tb_set_put(right, member_of(number + 20));
    }
    both = tb_set_union(left, right, &allocator);
    expect_set(both, (UINT64_C(1) << 60) - 1);
    /* Nothing is destroyed: the arena takes it all. */
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, heap_blocks);
    tb_arena_destroy(arena);
}

/* --- contract violations ------------------------------------------------------ */

static void ignore_member(const void *member, void *context)
{
    (void)member;
    (void)context;
}

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_set_length(NULL);
}

static void capacity_of_null(void *context)
{
    (void)context;
    (void)tb_set_capacity(NULL);
}

static void copy_of_null(void *context)
{
    (void)context;
    (void)tb_set_copy(NULL, NULL);
}

static void contains_in_null(void *context)
{
    (void)context;
    (void)tb_set_contains(NULL, member_of(1));
}

static void find_in_null(void *context)
{
    (void)context;
    (void)tb_set_find(NULL, member_of(1), NULL);
}

static void put_in_null(void *context)
{
    (void)context;
    (void)tb_set_put(NULL, member_of(1));
}

static void remove_from_null(void *context)
{
    (void)context;
    (void)tb_set_remove(NULL, member_of(1), NULL);
}

static void clear_null(void *context)
{
    (void)context;
    tb_set_clear(NULL);
}

static void visit_null(void *context)
{
    (void)context;
    tb_set_visit(NULL, ignore_member, NULL);
}

static void next_in_null(void *context)
{
    const void *member = NULL;
    size_t cursor = {0};

    (void)context;
    (void)tb_set_next(NULL, &cursor, &member);
}

static void to_array_of_null(void *context)
{
    (void)context;
    (void)tb_set_to_array(NULL, NULL, 0);
}

static void reserve_null(void *context)
{
    (void)context;
    tb_set_reserve(NULL, 1);
}

static void try_reserve_null(void *context)
{
    (void)context;
    (void)tb_set_try_reserve(NULL, 1);
}

static void shrink_null(void *context)
{
    (void)context;
    tb_set_shrink_to_fit(NULL);
}

TB_TEST(core_set, a_null_set_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        length_of_null,   capacity_of_null, copy_of_null,     contains_in_null, find_in_null,
        put_in_null,      remove_from_null, clear_null,       visit_null,       next_in_null,
        to_array_of_null, reserve_null,     try_reserve_null, shrink_null,
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], NULL, "set != NULL");
    }
}

/* A function of two sets, called with the pair in the context. */
typedef struct set_pair
{
    const tb_set *left;
    const tb_set *right;
} set_pair;

static void union_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_union(pair->left, pair->right, NULL);
}

static void intersection_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_intersection(pair->left, pair->right, NULL);
}

static void difference_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_difference(pair->left, pair->right, NULL);
}

static void symmetric_difference_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_symmetric_difference(pair->left, pair->right, NULL);
}

static void equal_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_equal(pair->left, pair->right);
}

static void is_subset_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_is_subset(pair->left, pair->right);
}

static void is_proper_subset_of_pair(void *context)
{
    const set_pair *const pair = context;

    (void)tb_set_is_proper_subset(pair->left, pair->right);
}

static tb_test_trap_body *const pair_functions[] = {
    union_of_pair, intersection_of_pair, difference_of_pair,       symmetric_difference_of_pair,
    equal_of_pair, is_subset_of_pair,    is_proper_subset_of_pair,
};

TB_TEST(core_set, a_null_operand_is_not_an_empty_set)
{
    tb_set *set = NULL;
    set_pair without_left = {0};
    set_pair without_right = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    set = tb_set_create(NULL, 0, NULL, NULL);
    without_left.right = set;
    without_right.left = set;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(pair_functions); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(pair_functions[index], &without_left, "left != NULL");
        TB_TEST_EXPECT_TRAP(pair_functions[index], &without_right, "right != NULL");
    }
    tb_set_destroy(set);
}

static uint64_t another_hash(const void *member)
{
    return tb_hash_pointer(member);
}

static bool another_equality(const void *left, const void *right)
{
    return left == right;
}

TB_TEST(core_set, sets_with_different_functions_cannot_be_combined)
{
    tb_set *by_address = NULL;
    tb_set *by_content = NULL;
    tb_set *other_hash = NULL;
    tb_set *other_equality = NULL;
    set_pair pairs[4] = {{0}};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    by_address = tb_set_create(NULL, 0, NULL, NULL);
    by_content = tb_set_create(NULL, 0, tb_set_string_hash, tb_set_string_equal);
    other_hash = tb_set_create(NULL, 0, another_hash, tb_set_string_equal);
    other_equality = tb_set_create(NULL, 0, tb_set_string_hash, another_equality);
    /* Sets of different sizes, so that no comparison can stop at the sizes
     * before it has looked at the functions. */
    (void)tb_set_put(by_content, "member");
    pairs[0] = (set_pair){.left = by_address, .right = by_content};
    pairs[1] = (set_pair){.left = by_content, .right = by_address};
    /* One function that differs is enough. */
    pairs[2] = (set_pair){.left = by_content, .right = other_hash};
    pairs[3] = (set_pair){.left = by_content, .right = other_equality};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(pair_functions); index++)
    {
        for (size_t pair = 0; pair < TB_TEST_COUNT_OF(pairs); pair++)
        {
            TB_TEST_CONTEXT("function %zu, pair %zu", index, pair);
            TB_TEST_EXPECT_TRAP(pair_functions[index], &pairs[pair],
                                "the sets must have the same hash and equality functions");
        }
    }
    tb_set_destroy(other_equality);
    tb_set_destroy(other_hash);
    tb_set_destroy(by_content);
    tb_set_destroy(by_address);
}

/* What a visitor does that it must not: change the set it is visiting. */
typedef enum intrusion
{
    INTRUSION_PUT,
    INTRUSION_REMOVE,
    INTRUSION_CLEAR,
    INTRUSION_RESERVE,
    INTRUSION_TRY_RESERVE,
    INTRUSION_SHRINK,
    INTRUSION_VISIT,
    INTRUSION_COUNT
} intrusion;

typedef struct intruder
{
    tb_set *set;
    intrusion what;
} intruder;

static void intrude(const void *member, void *context)
{
    const intruder *const visitor = context;

    switch (visitor->what)
    {
    case INTRUSION_PUT:
        (void)tb_set_put(visitor->set, member_of(999));
        break;
    case INTRUSION_REMOVE:
        (void)tb_set_remove(visitor->set, member, NULL);
        break;
    case INTRUSION_CLEAR:
        tb_set_clear(visitor->set);
        break;
    case INTRUSION_RESERVE:
        tb_set_reserve(visitor->set, 10000);
        break;
    case INTRUSION_TRY_RESERVE:
        (void)tb_set_try_reserve(visitor->set, 10000);
        break;
    case INTRUSION_SHRINK:
        tb_set_shrink_to_fit(visitor->set);
        break;
    case INTRUSION_VISIT:
        tb_set_visit(visitor->set, ignore_member, NULL);
        break;
    case INTRUSION_COUNT:
        break;
    }
}

static void visit_with_an_intruder(void *context)
{
    intruder *const visitor = context;

    tb_set_visit(visitor->set, intrude, visitor);
}

TB_TEST(core_set, changing_a_set_while_it_is_visited_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (int what = 0; what < INTRUSION_COUNT; what++)
    {
        /* A trap leaves the set in the middle of its visit, so each
         * intrusion gets a set of its own. */
        intruder visitor = {.set = tb_set_create(NULL, 0, NULL, NULL), .what = (intrusion)what};

        (void)tb_set_put(visitor.set, member_of(1));
        TB_TEST_CONTEXT("intrusion %d", what);
        TB_TEST_EXPECT_TRAP(visit_with_an_intruder, &visitor,
                            "the set must not be changed while it is visited");
        TB_TEST_EXPECT_UINT(tb_set_length(visitor.set), ==, 1);
        TB_TEST_EXPECT_UINT(tb_set_capacity(visitor.set), ==, 12);
        tb_set_destroy(visitor.set);
    }
}

static void create_with_only_a_hash(void *context)
{
    (void)context;
    (void)tb_set_create(NULL, 0, hash_to_zero, NULL);
}

static void create_with_only_an_equality(void *context)
{
    (void)context;
    (void)tb_set_create(NULL, 0, NULL, same_address);
}

static void visit_without_a_visitor(void *context)
{
    tb_set_visit(context, NULL, NULL);
}

static void to_array_without_an_array(void *context)
{
    (void)tb_set_to_array(context, NULL, 4);
}

static void next_without_a_cursor(void *context)
{
    const void *member = NULL;

    (void)tb_set_next(context, NULL, &member);
}

static void next_without_a_member(void *context)
{
    size_t cursor = {0};

    (void)tb_set_next(context, &cursor, NULL);
}

TB_TEST(core_set, missing_functions_and_arrays_violate_the_contract)
{
    tb_set *set = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    set = tb_set_create(NULL, 0, NULL, NULL);
    TB_TEST_EXPECT_TRAP(create_with_only_a_hash, NULL, "give a hash function and an equality");
    TB_TEST_EXPECT_TRAP(create_with_only_an_equality, NULL, "give a hash function and an equality");
    TB_TEST_EXPECT_TRAP(visit_without_a_visitor, set, "visitor != NULL");
    TB_TEST_EXPECT_TRAP(to_array_without_an_array, set, "members != NULL || capacity == 0");
    TB_TEST_EXPECT_TRAP(next_without_a_cursor, set, "cursor != NULL");
    TB_TEST_EXPECT_TRAP(next_without_a_member, set, "member != NULL");
    tb_set_destroy(set);
}
