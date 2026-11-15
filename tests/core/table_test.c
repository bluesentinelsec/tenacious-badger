/* A hash table hides its mistakes in the entries that collide: an entry that
 * can no longer be reached after its neighbor was removed, or after the table
 * grew. The cases therefore run the table with hash functions that collide on
 * purpose, as well as with the real one, and compare every answer with a
 * model that looks each key up directly. */
#include "badger/table.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

/* An integer as a key, and a key as the integer it was made from. */
static const void *key_of(size_t number)
{
    return (const void *)(uintptr_t)number;
}

/* A value that can be told apart from every other and from NULL. */
static void *value_of(size_t number)
{
    return (void *)(uintptr_t)(number * 2 + 1);
}

/* --- hash functions that collide on purpose ----------------------------------- */

static bool same_address(const void *left, const void *right)
{
    return left == right;
}

/* Every key in one slot: the table degenerates to one long run of entries. */
static uint64_t hash_to_zero(const void *key)
{
    (void)key;
    return 0;
}

/* A few slots, far apart, so that runs form, meet, and wrap around the end
 * of the array. */
static uint64_t hash_to_few_slots(const void *key)
{
    return ((uint64_t)(uintptr_t)key % 5) * UINT64_C(0x3333333333333333) + UINT64_MAX - 2;
}

/* A good hash that gives each key the hash of one neighbor: every slot that
 * is wanted is wanted twice. */
static uint64_t hash_in_pairs(const void *key)
{
    return tb_hash_u64((uint64_t)(uintptr_t)key / 2);
}

typedef struct hashing
{
    const char *name;
    tb_table_hash_function *hash;
    tb_table_equal_function *equal;
} hashing;

static const hashing hashings[] = {
    {"by address", NULL, NULL},
    {"every key in one slot", hash_to_zero, same_address},
    {"keys in five slots", hash_to_few_slots, same_address},
    {"keys that collide in pairs", hash_in_pairs, same_address},
};

/* --- the model ---------------------------------------------------------------- */

enum
{
    MODEL_KEY_LIMIT = 600
};

/* The reference: for every possible key, whether it is in the table and with
 * which value. */
typedef struct table_model
{
    bool is_present[MODEL_KEY_LIMIT];
    void *values[MODEL_KEY_LIMIT];
    size_t length;
} table_model;

/* Number of mistakes in a walk over the table: an entry the model does not
 * have, a wrong value, or a count that is not the model's. An entry returned
 * twice shows in the count. */
static size_t count_walk_differences(const tb_table *table, const table_model *model)
{
    size_t differences = {0};
    size_t walked = {0};
    size_t cursor = {0};
    tb_table_entry entry = {0};

    while (tb_table_next(table, &cursor, &entry))
    {
        const size_t number = (size_t)(uintptr_t)entry.key;

        walked++;
        if (number >= MODEL_KEY_LIMIT || !model->is_present[number] ||
            entry.value != model->values[number])
        {
            differences++;
        }
    }
    if (walked != model->length)
    {
        differences++;
    }
    return differences;
}

/* Number of keys for which the table and the model disagree, by every way of
 * asking the table. */
static size_t count_differences(const tb_table *table, const table_model *model)
{
    size_t differences = {0};

    if (tb_table_length(table) != model->length)
    {
        differences++;
    }
    for (size_t number = 0; number < MODEL_KEY_LIMIT; number++)
    {
        tb_table_entry entry = {0};
        const bool found = tb_table_find(table, key_of(number), &entry);

        if (found != model->is_present[number] || tb_table_contains(table, key_of(number)) != found)
        {
            differences++;
            continue;
        }
        if (!found)
        {
            if (tb_table_get(table, key_of(number)) != NULL)
            {
                differences++;
            }
            continue;
        }
        if (entry.key != key_of(number) || entry.value != model->values[number] ||
            tb_table_get(table, key_of(number)) != model->values[number])
        {
            differences++;
        }
    }
    return differences + count_walk_differences(table, model);
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_table, a_new_table_is_empty_and_allocates_only_what_it_was_told_to_expect)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    tb_table *const sized = tb_table_create(&allocator, 1000, NULL, NULL);
    size_t requests = {0};

    TB_TEST_REQUIRE_POINTER(table, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 0);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 0);
    TB_TEST_EXPECT(!tb_table_contains(table, key_of(1)));
    TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(1)), ==, NULL);
    TB_TEST_EXPECT(!tb_table_remove(table, key_of(1), NULL));
    TB_TEST_EXPECT_UINT(tb_table_to_array(table, NULL, 0), ==, 0);
    tb_table_clear(table);

    /* A table told what to expect holds that much without growing. */
    TB_TEST_EXPECT_UINT(tb_table_capacity(sized), >=, 1000);
    TB_TEST_EXPECT_UINT(tb_table_capacity(sized), <, 4000);
    requests = tracker.request_count;
    for (size_t number = 0; number < 1000; number++)
    {
        (void)tb_table_put(sized, key_of(number), value_of(number));
    }
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    TB_TEST_EXPECT_UINT(tb_table_length(sized), ==, 1000);

    /* The two tables, and the slots of the one that has any. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 3);
    tb_table_destroy(sized);
    tb_table_destroy(table);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_table, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_table_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- entries ------------------------------------------------------------------ */

TB_TEST(core_table, put_stores_a_value_and_returns_the_one_it_replaced)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_table *const table = tb_table_create(NULL, 0, hashings[row].hash, hashings[row].equal);
        tb_table_entry entry = {0};

        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(7), value_of(1)), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(8), value_of(2)), ==, NULL);
        TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 2);
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(7)), ==, value_of(1));
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(8)), ==, value_of(2));
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(9)), ==, NULL);

        /* The same key again replaces the value and adds no entry. */
        TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(7), value_of(3)), ==, value_of(1));
        TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 2);
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(7)), ==, value_of(3));

        TB_TEST_EXPECT(tb_table_remove(table, key_of(7), &entry));
        TB_TEST_EXPECT_POINTER(entry.key, ==, key_of(7));
        TB_TEST_EXPECT_POINTER(entry.value, ==, value_of(3));
        TB_TEST_EXPECT(!tb_table_remove(table, key_of(7), &entry));
        TB_TEST_EXPECT(!tb_table_contains(table, key_of(7)));
        TB_TEST_EXPECT(tb_table_contains(table, key_of(8)));
        TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 1);
        tb_table_destroy(table);
    }
}

TB_TEST(core_table, a_null_key_and_a_null_value_are_a_key_and_a_value)
{
    tb_table *const table = tb_table_create(NULL, 0, NULL, NULL);
    tb_table_entry entry = {.key = key_of(99), .value = value_of(99)};

    /* The integer zero is the NULL key, and it is as good as any other. */
    TB_TEST_EXPECT(!tb_table_contains(table, NULL));
    TB_TEST_EXPECT_POINTER(tb_table_put(table, NULL, value_of(5)), ==, NULL);
    TB_TEST_EXPECT(tb_table_contains(table, NULL));
    TB_TEST_EXPECT_POINTER(tb_table_get(table, NULL), ==, value_of(5));
    TB_TEST_EXPECT(!tb_table_contains(table, key_of(1)));

    /* A NULL value reads like a missing key through get, and find tells
     * them apart. */
    TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(1), NULL), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(1)), ==, NULL);
    TB_TEST_EXPECT(tb_table_find(table, key_of(1), &entry));
    TB_TEST_EXPECT_POINTER(entry.key, ==, key_of(1));
    TB_TEST_EXPECT_POINTER(entry.value, ==, NULL);
    /* A miss leaves the entry it was given untouched. */
    entry.value = value_of(99);
    TB_TEST_EXPECT(!tb_table_find(table, key_of(2), &entry));
    TB_TEST_EXPECT_POINTER(entry.key, ==, key_of(1));
    TB_TEST_EXPECT_POINTER(entry.value, ==, value_of(99));
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 2);
    TB_TEST_EXPECT(tb_table_remove(table, NULL, NULL));
    TB_TEST_EXPECT(!tb_table_contains(table, NULL));
    tb_table_destroy(table);
}

TB_TEST(core_table, string_keys_are_compared_by_content_and_the_first_key_is_kept)
{
    static const char *const words[] = {"", "a", "ab", "badger", "badgers", "Badger", "r_fov"};
    tb_table *const table = tb_table_create(NULL, 0, tb_table_string_hash, tb_table_string_equal);
    char first_copy[] = "badger";
    char second_copy[] = "badger";
    tb_table_entry entry = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(words); index++)
    {
        TB_TEST_EXPECT_POINTER(tb_table_put(table, words[index], value_of(index)), ==, NULL);
    }
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, TB_TEST_COUNT_OF(words));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(words); index++)
    {
        TB_TEST_CONTEXT("\"%s\"", words[index]);
        TB_TEST_EXPECT_POINTER(tb_table_get(table, words[index]), ==, value_of(index));
    }
    /* An equal string at another address is the same key. */
    TB_TEST_EXPECT_POINTER(tb_table_get(table, first_copy), ==, value_of(3));
    TB_TEST_EXPECT(!tb_table_contains(table, "badge"));
    TB_TEST_EXPECT(!tb_table_contains(table, "BADGER"));

    /* Putting through it replaces the value, and the key the table holds
     * stays the first one it was given. */
    TB_TEST_EXPECT_POINTER(tb_table_put(table, second_copy, value_of(50)), ==, value_of(3));
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, TB_TEST_COUNT_OF(words));
    TB_TEST_EXPECT(tb_table_find(table, first_copy, &entry));
    TB_TEST_EXPECT_POINTER(entry.key, ==, words[3]);
    TB_TEST_EXPECT_POINTER(entry.value, ==, value_of(50));
    TB_TEST_EXPECT(tb_table_remove(table, second_copy, &entry));
    TB_TEST_EXPECT_POINTER(entry.key, ==, words[3]);
    TB_TEST_EXPECT(!tb_table_contains(table, "badger"));
    TB_TEST_EXPECT_HEX(tb_table_string_hash("badger"), ==, tb_hash_string("badger"));
    tb_table_destroy(table);
}

TB_TEST(core_table, removing_an_entry_leaves_every_other_one_reachable)
{
    enum
    {
        KEY_COUNT = 11
    };

    /* With colliding hashes the entries form runs. Whichever entry of a run
     * is removed, the ones behind it must still be found, wherever the run
     * lies in the array, across its end included. */
    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        for (size_t removed = 0; removed < KEY_COUNT; removed++)
        {
            tb_table *const table =
                tb_table_create(NULL, 0, hashings[row].hash, hashings[row].equal);
            static table_model model = {0};

            memset(&model, 0, sizeof(model));
            for (size_t number = 0; number < KEY_COUNT; number++)
            {
                (void)tb_table_put(table, key_of(number), value_of(number));
                model.is_present[number] = true;
                model.values[number] = value_of(number);
                model.length++;
            }
            TB_TEST_CONTEXT("hashing %s, removing key %zu", hashings[row].name, removed);
            TB_TEST_EXPECT(tb_table_remove(table, key_of(removed), NULL));
            model.is_present[removed] = false;
            model.length--;
            TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
            /* And again from what is left, down to nothing. */
            for (size_t number = 0; number < KEY_COUNT; number++)
            {
                const size_t next = (removed + 1 + number * 4) % KEY_COUNT;

                if (model.is_present[next])
                {
                    TB_TEST_EXPECT(tb_table_remove(table, key_of(next), NULL));
                    model.is_present[next] = false;
                    model.length--;
                    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
                }
            }
            TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 0);
            tb_table_destroy(table);
        }
    }
}

TB_TEST(core_table, a_table_grows_to_hold_many_entries)
{
    enum
    {
        ENTRY_COUNT = 1000000,
        /* Sixteen slots, doubled seventeen times, hold them. */
        GROWTH_LIMIT = 18
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    size_t capacity = {0};
    size_t growth_count = {0};
    size_t wrong = {0};

    for (size_t number = 0; number < ENTRY_COUNT; number++)
    {
        (void)tb_table_put(table, key_of(number * 16), value_of(number));
        if (tb_table_capacity(table) != capacity)
        {
            TB_TEST_EXPECT_UINT(tb_table_capacity(table), >=, capacity * 2);
            capacity = tb_table_capacity(table);
            growth_count++;
        }
    }
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, ENTRY_COUNT);
    TB_TEST_EXPECT_UINT(growth_count, <=, GROWTH_LIMIT);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), >=, ENTRY_COUNT);
    /* The storage is a small multiple of what the entries need. */
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, <, (size_t)ENTRY_COUNT * 3 * sizeof(void *) * 3);
    for (size_t number = 0; number < ENTRY_COUNT; number++)
    {
        if (tb_table_get(table, key_of(number * 16)) != value_of(number) ||
            tb_table_contains(table, key_of(number * 16 + 1)))
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_table, clear_removes_every_entry_and_keeps_the_storage)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    size_t capacity = {0};
    size_t requests = {0};

    for (size_t number = 0; number < 500; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
    }
    capacity = tb_table_capacity(table);
    requests = tracker.request_count;
    tb_table_clear(table);
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 0);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, capacity);
    TB_TEST_EXPECT(!tb_table_contains(table, key_of(250)));
    for (size_t number = 0; number < 500; number++)
    {
        TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(number), value_of(number + 1)), ==, NULL);
    }
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(250)), ==, value_of(251));
    tb_table_destroy(table);
}

TB_TEST(core_table, entries_that_come_and_go_at_a_constant_size_leave_nothing_behind)
{
    enum
    {
        LIVE_COUNT = 1000,
        CYCLE_COUNT = 200000
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, LIVE_COUNT, NULL, NULL);
    size_t requests = {0};
    size_t capacity = {0};
    size_t wrong = {0};

    for (size_t number = 0; number < LIVE_COUNT; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
    }
    requests = tracker.request_count;
    capacity = tb_table_capacity(table);
    /* Each cycle removes the oldest key and adds a new one. A table that left
     * a marker in place of every removed entry would fill up with markers and
     * have to grow, though it never holds more than it did at the start. */
    for (size_t cycle = 0; cycle < CYCLE_COUNT; cycle++)
    {
        if (!tb_table_remove(table, key_of(cycle), NULL))
        {
            wrong++;
        }
        if (tb_table_put(table, key_of(cycle + LIVE_COUNT), value_of(cycle + LIVE_COUNT)) != NULL)
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, LIVE_COUNT);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, capacity);
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    for (size_t number = CYCLE_COUNT; number < CYCLE_COUNT + LIVE_COUNT; number++)
    {
        if (tb_table_get(table, key_of(number)) != value_of(number))
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    TB_TEST_EXPECT(!tb_table_contains(table, key_of(CYCLE_COUNT - 1)));
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_table, shrink_to_fit_gives_back_the_slots_the_entries_do_not_need)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    static table_model model = {0};
    size_t bytes_before = {0};
    size_t requests = {0};

    memset(&model, 0, sizeof(model));
    for (size_t number = 0; number < 500; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
    }
    for (size_t number = 20; number < 500; number++)
    {
        (void)tb_table_remove(table, key_of(number), NULL);
    }
    for (size_t number = 0; number < 20; number++)
    {
        model.is_present[number] = true;
        model.values[number] = value_of(number);
        model.length++;
    }
    /* Removing gave nothing back. */
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), >=, 500);
    bytes_before = tracker.live_byte_count;
    tb_table_shrink_to_fit(table);
    /* Twenty entries need 32 slots, which hold 24. */
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, <, bytes_before / 8);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 2);
    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);

    /* Already as small as it can be: the allocator is not asked again. */
    requests = tracker.request_count;
    tb_table_shrink_to_fit(table);
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);

    /* When the smaller block cannot be had, the table stays as it was. */
    for (size_t number = 5; number < 20; number++)
    {
        (void)tb_table_remove(table, key_of(number), NULL);
        model.is_present[number] = false;
        model.length--;
    }
    tb_test_allocator_fail_after(&tracker, 0);
    tb_table_shrink_to_fit(table);
    tb_test_allocator_stop_failing(&tracker);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);
    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
    tb_table_shrink_to_fit(table);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 12);
    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);

    /* An empty table keeps no slots at all, and can be filled again. */
    tb_table_clear(table);
    tb_table_shrink_to_fit(table);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 0);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    tb_table_shrink_to_fit(table);
    TB_TEST_EXPECT(!tb_table_contains(table, key_of(1)));
    TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(1), value_of(1)), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(1)), ==, value_of(1));
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_table, reserve_makes_exactly_the_room_that_is_needed)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    tb_table *const sized = tb_table_create(&allocator, 12, NULL, NULL);
    size_t requests = {0};

    /* Sixteen slots hold twelve entries, so room for twelve is sixteen
     * slots and room for thirteen is thirty-two. */
    TB_TEST_EXPECT_UINT(tb_table_capacity(sized), ==, 12);
    tb_table_reserve(table, 1);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 12);
    requests = tracker.request_count;
    tb_table_reserve(table, 12);
    TB_TEST_EXPECT(tb_table_try_reserve(table, 12));
    tb_table_reserve(table, 0);
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 12);
    tb_table_reserve(table, 13);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);
    tb_table_reserve(table, 24);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);
    TB_TEST_EXPECT(tb_table_try_reserve(table, 25));
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 48);
    /* The entries keep their places in the count through all of it. */
    for (size_t number = 0; number < 48; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
    }
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 48);
    tb_table_reserve(table, 1000);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 1536);
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 48);
    for (size_t number = 0; number < 48; number++)
    {
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(number)), ==, value_of(number));
    }
    tb_table_destroy(sized);
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- copies ------------------------------------------------------------------- */

TB_TEST(core_table, a_copy_has_the_same_entries_and_a_life_of_its_own)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_table *const table = tb_table_create(NULL, 0, hashings[row].hash, hashings[row].equal);
        tb_table *copy = NULL;
        static table_model model = {0};

        memset(&model, 0, sizeof(model));
        for (size_t number = 0; number < 300; number += 3)
        {
            (void)tb_table_put(table, key_of(number), value_of(number));
            model.is_present[number] = true;
            model.values[number] = value_of(number);
            model.length++;
        }
        /* Room the original has to spare is not copied. */
        tb_table_reserve(table, 10000);
        copy = tb_table_copy(table, &allocator);
        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        TB_TEST_EXPECT_UINT(count_differences(copy, &model), ==, 0);
        /* A hundred entries need 256 slots, which hold 192. */
        TB_TEST_EXPECT_UINT(tb_table_capacity(copy), ==, 192);
        /* The copy is in the allocator it was given: the table and its
         * slots. */
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 2);

        /* Changing the copy leaves the original as it was. */
        (void)tb_table_remove(copy, key_of(0), NULL);
        (void)tb_table_put(copy, key_of(1), value_of(1));
        (void)tb_table_put(copy, key_of(3), value_of(4));
        TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
        /* And changing the original leaves the copy. */
        tb_table_clear(table);
        model.is_present[0] = false;
        model.is_present[1] = true;
        model.values[1] = value_of(1);
        model.values[3] = value_of(4);
        TB_TEST_EXPECT_UINT(count_differences(copy, &model), ==, 0);

        tb_table_destroy(copy);
        tb_table_destroy(table);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    }
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_table, a_copy_compares_keys_as_the_original_does)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(NULL, 0, tb_table_string_hash, tb_table_string_equal);
    tb_table *const empty_copy = tb_table_copy(table, &allocator);
    tb_table *copy = NULL;
    char same_content[] = "badger";

    /* The copy of an empty table has no slots. */
    TB_TEST_EXPECT_UINT(tb_table_length(empty_copy), ==, 0);
    TB_TEST_EXPECT_UINT(tb_table_capacity(empty_copy), ==, 0);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    (void)tb_table_put(table, "badger", value_of(1));
    copy = tb_table_copy(table, NULL);
    TB_TEST_EXPECT_POINTER(tb_table_get(copy, same_content), ==, value_of(1));
    TB_TEST_EXPECT_POINTER(tb_table_put(copy, same_content, value_of(2)), ==, value_of(1));
    TB_TEST_EXPECT_UINT(tb_table_length(copy), ==, 1);
    /* So does the copy of the empty table, once it has entries. */
    (void)tb_table_put(empty_copy, "badger", value_of(3));
    TB_TEST_EXPECT_POINTER(tb_table_get(empty_copy, same_content), ==, value_of(3));
    tb_table_destroy(copy);
    tb_table_destroy(empty_copy);
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- all entries -------------------------------------------------------------- */

typedef struct visit_record
{
    size_t seen[MODEL_KEY_LIMIT];
    size_t call_count;
    size_t wrong_value_count;
} visit_record;

/* Counts each key, checks its value, and stores the next value in its place. */
static void record_and_bump(const void *key, void **value, void *context)
{
    visit_record *const record = context;
    const size_t number = (size_t)(uintptr_t)key;

    record->call_count++;
    record->seen[number]++;
    if (*value != value_of(number))
    {
        record->wrong_value_count++;
    }
    *value = value_of(number + 1);
}

TB_TEST(core_table, visit_reaches_every_entry_once_and_may_change_its_value)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_table *const table = tb_table_create(NULL, 0, hashings[row].hash, hashings[row].equal);
        static visit_record record = {0};
        size_t unvisited = {0};

        memset(&record, 0, sizeof(record));
        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        /* Visiting an empty table calls nothing. */
        tb_table_visit(table, record_and_bump, &record);
        TB_TEST_EXPECT_UINT(record.call_count, ==, 0);
        for (size_t number = 0; number < 300; number += 3)
        {
            (void)tb_table_put(table, key_of(number), value_of(number));
        }
        tb_table_visit(table, record_and_bump, &record);
        TB_TEST_EXPECT_UINT(record.call_count, ==, 100);
        TB_TEST_EXPECT_UINT(record.wrong_value_count, ==, 0);
        for (size_t number = 0; number < 300; number++)
        {
            if (record.seen[number] != (number % 3 == 0 ? 1u : 0u))
            {
                unvisited++;
            }
        }
        TB_TEST_EXPECT_UINT(unvisited, ==, 0);
        /* The values the visitor stored are the values of the table now. */
        TB_TEST_EXPECT_POINTER(tb_table_get(table, key_of(30)), ==, value_of(31));
        tb_table_destroy(table);
    }
}

TB_TEST(core_table, next_steps_through_every_entry_once)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(hashings); row++)
    {
        tb_table *const table = tb_table_create(NULL, 0, hashings[row].hash, hashings[row].equal);
        tb_table_entry entry = {.key = key_of(77), .value = value_of(77)};
        size_t seen[300] = {0};
        size_t cursor = {0};
        size_t count = {0};
        size_t wrong = {0};

        TB_TEST_CONTEXT("hashing %s", hashings[row].name);
        /* An empty table has no next entry, and leaves the entry alone. */
        TB_TEST_EXPECT(!tb_table_next(table, &cursor, &entry));
        TB_TEST_EXPECT_POINTER(entry.key, ==, key_of(77));
        TB_TEST_EXPECT_POINTER(entry.value, ==, value_of(77));
        for (size_t number = 0; number < 300; number += 3)
        {
            (void)tb_table_put(table, key_of(number), value_of(number));
        }
        cursor = 0;
        while (tb_table_next(table, &cursor, &entry))
        {
            const size_t number = (size_t)(uintptr_t)entry.key;

            TB_TEST_REQUIRE_UINT(number, <, 300);
            seen[number]++;
            count++;
            if (entry.value != value_of(number))
            {
                wrong++;
            }
        }
        TB_TEST_EXPECT_UINT(count, ==, 100);
        for (size_t number = 0; number < 300; number++)
        {
            if (seen[number] != (number % 3 == 0 ? 1u : 0u))
            {
                wrong++;
            }
        }
        TB_TEST_EXPECT_UINT(wrong, ==, 0);
        /* Asked again after the end, it keeps answering that there are no
         * more, and still leaves the entry alone. */
        entry.key = key_of(77);
        entry.value = value_of(77);
        TB_TEST_EXPECT(!tb_table_next(table, &cursor, &entry));
        TB_TEST_EXPECT(!tb_table_next(table, &cursor, &entry));
        TB_TEST_EXPECT_POINTER(entry.key, ==, key_of(77));
        TB_TEST_EXPECT_POINTER(entry.value, ==, value_of(77));
        tb_table_destroy(table);
    }
}

TB_TEST(core_table, to_array_copies_the_entries_that_fit_and_reports_the_length)
{
    static const size_t capacities[] = {0, 1, 5, 9, 10, 11, 20};
    tb_table *const table = tb_table_create(NULL, 0, NULL, NULL);

    for (size_t number = 0; number < 10; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(capacities); row++)
    {
        tb_table_entry entries[21] = {0};
        size_t seen[10] = {0};
        const size_t capacity = capacities[row];
        const size_t copied = capacity < 10 ? capacity : 10;

        TB_TEST_CONTEXT("capacity %zu", capacity);
        memset(entries, 0xFF, sizeof(entries));
        TB_TEST_EXPECT_UINT(tb_table_to_array(table, entries, capacity), ==, 10);
        for (size_t index = 0; index < copied; index++)
        {
            const size_t number = (size_t)(uintptr_t)entries[index].key;

            TB_TEST_REQUIRE_UINT(number, <, 10);
            TB_TEST_EXPECT_POINTER(entries[index].value, ==, value_of(number));
            seen[number]++;
            /* Each entry is copied at most once. */
            TB_TEST_EXPECT_UINT(seen[number], ==, 1);
        }
        /* Nothing is written beyond the capacity or beyond the entries. */
        TB_TEST_EXPECT_POINTER(entries[copied].value, ==, entries[20].value);
        TB_TEST_EXPECT_POINTER(entries[copied].key, ==, entries[20].key);
    }
    tb_table_destroy(table);
}

/* --- random operations against a model ---------------------------------------- */

static void run_model(const tb_allocator *allocator, const hashing *how)
{
    enum
    {
        OPERATION_COUNT = 30000
    };
    static table_model model = {0};
    tb_table *const table = tb_table_create(allocator, 0, how->hash, how->equal);
    size_t removal_count = {0};
    size_t replacement_count = {0};

    memset(&model, 0, sizeof(model));
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(1000);
        /* Keys from a small range at first, so that removals and
         * replacements are common, and from the whole range later. */
        const size_t range = operation < OPERATION_COUNT / 2 ? 40 : MODEL_KEY_LIMIT;
        const size_t number = (size_t)tb_test_random_below(range);
        void *const value = value_of((size_t)tb_test_random_below(1000));
        tb_table_entry entry = {0};

        TB_TEST_CONTEXT("hashing %s, operation %zu (choice %u) on key %zu, %zu entries", how->name,
                        operation, (unsigned)choice, number, model.length);
        if (choice < 550)
        {
            void *const previous = tb_table_put(table, key_of(number), value);

            if (model.is_present[number])
            {
                TB_TEST_EXPECT_POINTER(previous, ==, model.values[number]);
                replacement_count++;
            }
            else
            {
                TB_TEST_EXPECT_POINTER(previous, ==, NULL);
                model.is_present[number] = true;
                model.length++;
            }
            model.values[number] = value;
        }
        else if (choice < 940)
        {
            const bool removed = tb_table_remove(table, key_of(number), &entry);

            TB_TEST_EXPECT(removed == model.is_present[number]);
            if (removed)
            {
                TB_TEST_EXPECT_POINTER(entry.value, ==, model.values[number]);
                model.is_present[number] = false;
                model.length--;
                removal_count++;
            }
        }
        else if (choice < 942)
        {
            tb_table_clear(table);
            memset(&model, 0, sizeof(model));
        }
        else if (choice < 970)
        {
            tb_table_reserve(table, model.length + (size_t)tb_test_random_below(200));
        }
        else
        {
            tb_table_shrink_to_fit(table);
        }
        TB_TEST_REQUIRE_UINT(tb_table_length(table), ==, model.length);
        TB_TEST_REQUIRE_UINT(tb_table_capacity(table), >=, model.length);
        TB_TEST_REQUIRE(tb_table_contains(table, key_of(number)) == model.is_present[number]);
        /* Every key is checked now and then, since an operation on one key
         * can lose another. */
        if (operation % 50 == 0)
        {
            TB_TEST_REQUIRE_UINT(count_differences(table, &model), ==, 0);
        }
    }
    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
    TB_TEST_EXPECT_UINT(removal_count, >, OPERATION_COUNT / 10);
    TB_TEST_EXPECT_UINT(replacement_count, >, OPERATION_COUNT / 20);
    tb_table_destroy(table);
}

TB_TEST(core_table, random_operations_agree_with_a_direct_lookup)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    run_model(&allocator, &hashings[0]);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_table, random_operations_agree_when_every_key_shares_a_slot)
{
    run_model(NULL, &hashings[1]);
}

TB_TEST(core_table, random_operations_agree_when_keys_crowd_into_a_few_slots)
{
    run_model(NULL, &hashings[2]);
}

TB_TEST(core_table, random_operations_agree_when_keys_collide_in_pairs)
{
    run_model(NULL, &hashings[3]);
}

TB_TEST(core_table, random_operations_agree_for_a_table_in_an_arena)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);

    run_model(&allocator, &hashings[0]);
    tb_arena_destroy(arena);
}

/* --- storage and running out of memory ---------------------------------------- */

static void put_a_new_key(void *context)
{
    (void)tb_table_put(context, key_of(100000), value_of(1));
}

static void reserve_a_million(void *context)
{
    tb_table_reserve(context, 1000000);
}

static void reserve_more_than_memory_can_hold(void *context)
{
    tb_table_reserve(context, SIZE_MAX);
}

static void create_a_table(void *context)
{
    (void)tb_table_create(context, 0, NULL, NULL);
}

static void create_a_table_with_room(void *context)
{
    (void)tb_table_create(context, 100, NULL, NULL);
}

TB_TEST(core_table, an_exhausted_allocator_ends_growth_and_leaves_the_table_intact)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_table *const table = tb_table_create(&allocator, 0, NULL, NULL);
    static table_model model = {0};
    size_t requests = {0};
    size_t live_blocks = {0};

    memset(&model, 0, sizeof(model));
    /* Filled to its capacity, so that one more key must grow it. */
    for (size_t number = 0; number < 12; number++)
    {
        (void)tb_table_put(table, key_of(number), value_of(number));
        model.is_present[number] = true;
        model.values[number] = value_of(number);
        model.length++;
    }
    TB_TEST_REQUIRE_UINT(tb_table_capacity(table), ==, 12);
    live_blocks = tracker.live_block_count;
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(put_a_new_key, table, "out of memory");
    TB_TEST_EXPECT_TRAP(reserve_a_million, table, "out of memory: 1000000 x");
    TB_TEST_EXPECT_TRAP(create_a_table, &allocator, "out of memory");
    TB_TEST_EXPECT(!tb_table_try_reserve(table, 13));
    /* A key that is there is replaced without any memory. */
    TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(3), value_of(33)), ==, value_of(3));
    model.values[3] = value_of(33);
    tb_test_allocator_stop_failing(&tracker);
    /* A table that cannot get its first slots is not left half made. */
    tb_test_allocator_fail_after(&tracker, 1);
    TB_TEST_EXPECT_TRAP(create_a_table_with_room, &allocator, "out of memory: 100 x");
    tb_test_allocator_stop_failing(&tracker);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, live_blocks);

    TB_TEST_EXPECT_UINT(count_differences(table, &model), ==, 0);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 12);

    /* A capacity that cannot be expressed is refused without asking. */
    requests = tracker.request_count;
    TB_TEST_EXPECT(!tb_table_try_reserve(table, SIZE_MAX));
    TB_TEST_EXPECT(!tb_table_try_reserve(table, SIZE_MAX / 2));
    TB_TEST_EXPECT_TRAP(reserve_more_than_memory_can_hold, table, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.request_count, ==, requests);

    /* With memory to be had again, the table grows. */
    TB_TEST_EXPECT_POINTER(tb_table_put(table, key_of(500), value_of(500)), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_table_length(table), ==, 13);
    TB_TEST_EXPECT_UINT(tb_table_capacity(table), ==, 24);
    tb_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

static void ignore_entry(const void *key, void **value, void *context)
{
    (void)key;
    (void)value;
    (void)context;
}

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_table_length(NULL);
}

static void capacity_of_null(void *context)
{
    (void)context;
    (void)tb_table_capacity(NULL);
}

static void put_in_null(void *context)
{
    (void)context;
    (void)tb_table_put(NULL, key_of(1), NULL);
}

static void get_from_null(void *context)
{
    (void)context;
    (void)tb_table_get(NULL, key_of(1));
}

static void find_in_null(void *context)
{
    (void)context;
    (void)tb_table_find(NULL, key_of(1), NULL);
}

static void contains_in_null(void *context)
{
    (void)context;
    (void)tb_table_contains(NULL, key_of(1));
}

static void remove_from_null(void *context)
{
    (void)context;
    (void)tb_table_remove(NULL, key_of(1), NULL);
}

static void clear_null(void *context)
{
    (void)context;
    tb_table_clear(NULL);
}

static void visit_null(void *context)
{
    (void)context;
    tb_table_visit(NULL, ignore_entry, NULL);
}

static void to_array_of_null(void *context)
{
    (void)context;
    (void)tb_table_to_array(NULL, NULL, 0);
}

static void reserve_null(void *context)
{
    (void)context;
    tb_table_reserve(NULL, 1);
}

static void try_reserve_null(void *context)
{
    (void)context;
    (void)tb_table_try_reserve(NULL, 1);
}

static void copy_of_null(void *context)
{
    (void)context;
    (void)tb_table_copy(NULL, NULL);
}

static void shrink_null(void *context)
{
    (void)context;
    tb_table_shrink_to_fit(NULL);
}

static void next_in_null(void *context)
{
    size_t cursor = {0};
    tb_table_entry entry = {0};

    (void)context;
    (void)tb_table_next(NULL, &cursor, &entry);
}

TB_TEST(core_table, a_null_table_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        length_of_null,   capacity_of_null, put_in_null, get_from_null, find_in_null,
        contains_in_null, remove_from_null, clear_null,  visit_null,    to_array_of_null,
        reserve_null,     try_reserve_null, shrink_null, next_in_null,  copy_of_null,
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], NULL, "table != NULL");
    }
}

/* What a visitor does that it must not: change the table it is visiting. */
typedef enum intrusion
{
    INTRUSION_PUT,
    INTRUSION_REPLACE,
    INTRUSION_REMOVE,
    INTRUSION_CLEAR,
    INTRUSION_RESERVE,
    INTRUSION_SHRINK,
    INTRUSION_VISIT,
    INTRUSION_COUNT
} intrusion;

typedef struct intruder
{
    tb_table *table;
    intrusion what;
} intruder;

static void intrude(const void *key, void **value, void *context)
{
    const intruder *const visitor = context;

    (void)value;
    switch (visitor->what)
    {
    case INTRUSION_PUT:
        (void)tb_table_put(visitor->table, key_of(999), NULL);
        break;
    case INTRUSION_REPLACE:
        /* Even over a key that is there, which adds no entry: the visitor
         * has the pointer it was given for that. */
        (void)tb_table_put(visitor->table, key, NULL);
        break;
    case INTRUSION_REMOVE:
        (void)tb_table_remove(visitor->table, key, NULL);
        break;
    case INTRUSION_CLEAR:
        tb_table_clear(visitor->table);
        break;
    case INTRUSION_RESERVE:
        tb_table_reserve(visitor->table, 10000);
        break;
    case INTRUSION_SHRINK:
        tb_table_shrink_to_fit(visitor->table);
        break;
    case INTRUSION_VISIT:
        tb_table_visit(visitor->table, ignore_entry, NULL);
        break;
    case INTRUSION_COUNT:
        break;
    }
}

static void visit_with_an_intruder(void *context)
{
    intruder *const visitor = context;

    tb_table_visit(visitor->table, intrude, visitor);
}

TB_TEST(core_table, changing_a_table_while_it_is_visited_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (int what = 0; what < INTRUSION_COUNT; what++)
    {
        /* A trap leaves the table in the middle of its visit, so each
         * intrusion gets a table of its own. */
        intruder visitor = {.table = tb_table_create(NULL, 0, NULL, NULL), .what = (intrusion)what};

        (void)tb_table_put(visitor.table, key_of(1), value_of(1));
        TB_TEST_CONTEXT("intrusion %d", what);
        TB_TEST_EXPECT_TRAP(visit_with_an_intruder, &visitor,
                            "the table must not be changed while it is visited");
        TB_TEST_EXPECT_UINT(tb_table_length(visitor.table), ==, 1);
        TB_TEST_EXPECT_POINTER(tb_table_get(visitor.table, key_of(1)), ==, value_of(1));
        tb_table_destroy(visitor.table);
    }
}

static void create_with_only_a_hash(void *context)
{
    (void)context;
    (void)tb_table_create(NULL, 0, hash_to_zero, NULL);
}

static void create_with_only_an_equality(void *context)
{
    (void)context;
    (void)tb_table_create(NULL, 0, NULL, same_address);
}

static void visit_without_a_visitor(void *context)
{
    tb_table_visit(context, NULL, NULL);
}

static void to_array_without_an_array(void *context)
{
    (void)tb_table_to_array(context, NULL, 4);
}

static void next_without_a_cursor(void *context)
{
    tb_table_entry entry = {0};

    (void)tb_table_next(context, NULL, &entry);
}

static void next_without_an_entry(void *context)
{
    size_t cursor = {0};

    (void)tb_table_next(context, &cursor, NULL);
}

static void compare_a_null_string(void *context)
{
    (void)context;
    (void)tb_table_string_equal("a", NULL);
}

static void compare_with_a_null_string(void *context)
{
    (void)context;
    (void)tb_table_string_equal(NULL, "a");
}

TB_TEST(core_table, missing_functions_and_arrays_violate_the_contract)
{
    tb_table *table = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    table = tb_table_create(NULL, 0, NULL, NULL);
    TB_TEST_EXPECT_TRAP(create_with_only_a_hash, NULL, "give a hash function and an equality");
    TB_TEST_EXPECT_TRAP(create_with_only_an_equality, NULL, "give a hash function and an equality");
    TB_TEST_EXPECT_TRAP(visit_without_a_visitor, table, "visitor != NULL");
    TB_TEST_EXPECT_TRAP(to_array_without_an_array, table, "entries != NULL || capacity == 0");
    TB_TEST_EXPECT_TRAP(next_without_a_cursor, table, "cursor != NULL");
    TB_TEST_EXPECT_TRAP(next_without_an_entry, table, "entry != NULL");
    TB_TEST_EXPECT_TRAP(compare_a_null_string, NULL, "right != NULL");
    TB_TEST_EXPECT_TRAP(compare_with_a_null_string, NULL, "left != NULL");
    tb_table_destroy(table);
}
