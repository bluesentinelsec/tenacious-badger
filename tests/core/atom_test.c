/* The whole value of an atom is one promise: equal bytes give the same
 * pointer, and different bytes a different one. The cases press on that
 * promise where a hash table breaks it: bytes that differ only at their end
 * or only in length, strings built to land in the same slot, and enough
 * atoms to force the table to grow many times over. */
#include "badger/atom.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Checks an atom against the bytes it was made from: its length, its bytes,
 * its terminator, and its hash. */
static void expect_atom(const char *atom, const char *bytes, size_t length)
{
    TB_TEST_REQUIRE_POINTER(atom, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_atom_length(atom), ==, length);
    if (length > 0)
    {
        TB_TEST_EXPECT_BYTES(atom, bytes, length);
    }
    TB_TEST_EXPECT_INT(atom[length], ==, '\0');
    TB_TEST_EXPECT_HEX(tb_atom_hash(atom), ==, tb_hash_bytes(bytes, length));
}

/* --- tables ------------------------------------------------------------------- */

TB_TEST(core_atom, a_new_table_is_empty_and_holds_no_atoms)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);

    TB_TEST_REQUIRE_POINTER(table, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 0);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "name", 4), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, NULL, 0), ==, NULL);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_atom, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_atom_table_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- interning ---------------------------------------------------------------- */

TB_TEST(core_atom, equal_bytes_give_the_same_pointer)
{
    tb_atom_table *const table = tb_atom_table_create(NULL);
    char first_copy[] = "r_fov";
    char second_copy[] = "r_fov";
    const char *const from_first = tb_atom_string(table, first_copy);
    const char *const from_second = tb_atom_string(table, second_copy);
    const char *const from_bytes = tb_atom_new(table, "r_fov and more", 5);

    TB_TEST_EXPECT_POINTER(from_first, ==, from_second);
    TB_TEST_EXPECT_POINTER(from_first, ==, from_bytes);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 1);
    expect_atom(from_first, "r_fov", 5);
    /* The atom is the table's own copy, not the caller's string. */
    TB_TEST_EXPECT_POINTER(from_first, !=, first_copy);
    first_copy[0] = 'X';
    TB_TEST_EXPECT_STRING(from_first, ==, "r_fov");
    TB_TEST_EXPECT_POINTER(tb_atom_string(table, "r_fov"), ==, from_first);
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, any_difference_in_bytes_or_length_gives_a_different_pointer)
{
    static const char *const names[] = {
        "",       "a",       "b",     "A",           "aa",     "ab",    "ba",
        "r_fov",  "r_fow",   "r_fo",  "r_fov ",      " r_fov", "R_FOV", "r_fov2",
        "player", "players", "playe", "caf\xC3\xA9", "cafe",   "0",     "00",
    };
    tb_atom_table *const table = tb_atom_table_create(NULL);
    const char *atoms[TB_TEST_COUNT_OF(names)] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        atoms[index] = tb_atom_string(table, names[index]);
        TB_TEST_CONTEXT("\"%s\"", names[index]);
        expect_atom(atoms[index], names[index], strlen(names[index]));
        for (size_t other = 0; other < index; other++)
        {
            TB_TEST_EXPECT_POINTER(atoms[index], !=, atoms[other]);
        }
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, TB_TEST_COUNT_OF(names));
    /* Interning them all again finds the same atoms and adds none. */
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        TB_TEST_EXPECT_POINTER(tb_atom_string(table, names[index]), ==, atoms[index]);
        TB_TEST_EXPECT_POINTER(tb_atom_find(table, names[index], strlen(names[index])), ==,
                               atoms[index]);
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, TB_TEST_COUNT_OF(names));
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, every_byte_value_and_every_prefix_is_its_own_atom)
{
    enum
    {
        BYTE_COUNT = 256
    };
    tb_atom_table *const table = tb_atom_table_create(NULL);
    const char *single[BYTE_COUNT] = {0};
    const char *prefixes[BYTE_COUNT + 1] = {0};
    char bytes[BYTE_COUNT] = {0};

    for (size_t value = 0; value < BYTE_COUNT; value++)
    {
        bytes[value] = (char)(unsigned char)value;
    }
    /* One byte of each value, the zero byte included. */
    for (size_t value = 0; value < BYTE_COUNT; value++)
    {
        single[value] = tb_atom_new(table, &bytes[value], 1);
        TB_TEST_CONTEXT("byte %zu", value);
        expect_atom(single[value], &bytes[value], 1);
    }
    /* Every prefix of the same 256 bytes, the empty one included. The first
     * byte is zero, so each of these has a zero byte inside it. */
    for (size_t length = 0; length <= BYTE_COUNT; length++)
    {
        prefixes[length] = tb_atom_new(table, bytes, length);
        TB_TEST_CONTEXT("prefix of %zu bytes", length);
        expect_atom(prefixes[length], bytes, length);
        if (length > 0)
        {
            TB_TEST_EXPECT_POINTER(prefixes[length], !=, prefixes[length - 1]);
        }
    }
    /* The prefix of one byte is the single zero byte interned before. */
    TB_TEST_EXPECT_POINTER(prefixes[1], ==, single[0]);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 2 * BYTE_COUNT);
    TB_TEST_EXPECT_POINTER(tb_atom_new(table, NULL, 0), ==, prefixes[0]);
    TB_TEST_EXPECT_POINTER(tb_atom_string(table, ""), ==, prefixes[0]);
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, a_very_long_string_is_an_atom_like_any_other)
{
    enum
    {
        LENGTH = 1024 * 1024
    };
    tb_atom_table *const table = tb_atom_table_create(NULL);
    char *const bytes = tb_malloc(LENGTH);
    const char *atom = NULL;
    const char *changed = NULL;

    for (size_t index = 0; index < LENGTH; index++)
    {
        bytes[index] = (char)('a' + index % 26);
    }
    atom = tb_atom_new(table, bytes, LENGTH);
    expect_atom(atom, bytes, LENGTH);
    TB_TEST_EXPECT_POINTER(tb_atom_new(table, bytes, LENGTH), ==, atom);
    /* A megabyte that differs only in its last byte is another atom. */
    bytes[LENGTH - 1] = '!';
    changed = tb_atom_new(table, bytes, LENGTH);
    TB_TEST_EXPECT_POINTER(changed, !=, atom);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 2);
    tb_free(bytes);
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, an_integer_is_interned_as_its_decimal_digits)
{
    typedef struct integer_row
    {
        long long value;
        const char *digits;
    } integer_row;
    static const integer_row rows[] = {
        {0, "0"},
        {1, "1"},
        {-1, "-1"},
        {42, "42"},
        {-1000000, "-1000000"},
        {LLONG_MAX, "9223372036854775807"},
        {LLONG_MIN, "-9223372036854775808"},
    };
    tb_atom_table *const table = tb_atom_table_create(NULL);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const char *const atom = tb_atom_int(table, rows[index].value);

        TB_TEST_CONTEXT("%lld", rows[index].value);
        TB_TEST_EXPECT_STRING(atom, ==, rows[index].digits);
        /* The same atom as the digits written out. */
        TB_TEST_EXPECT_POINTER(atom, ==, tb_atom_string(table, rows[index].digits));
        TB_TEST_EXPECT_POINTER(atom, ==, tb_atom_int(table, rows[index].value));
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, TB_TEST_COUNT_OF(rows));
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, find_looks_without_creating)
{
    tb_atom_table *const table = tb_atom_table_create(NULL);
    const char *const known = tb_atom_string(table, "known");

    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "known", 5), ==, known);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "unknown", 7), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "know", 4), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "known!", 6), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, "", 0), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 1);
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, tables_are_independent)
{
    tb_atom_table *const first = tb_atom_table_create(NULL);
    tb_atom_table *const second = tb_atom_table_create(NULL);
    const char *const in_first = tb_atom_string(first, "shared");
    const char *const in_second = tb_atom_string(second, "shared");

    TB_TEST_EXPECT_POINTER(in_first, !=, in_second);
    TB_TEST_EXPECT_STRING(in_first, ==, in_second);
    TB_TEST_EXPECT_POINTER(tb_atom_find(second, "only_first", 10), ==, NULL);
    /* Destroying one table leaves the atoms of the other valid. */
    tb_atom_table_destroy(first);
    TB_TEST_EXPECT_STRING(in_second, ==, "shared");
    TB_TEST_EXPECT_POINTER(tb_atom_string(second, "shared"), ==, in_second);
    tb_atom_table_destroy(second);
}

/* --- pressure on the table ---------------------------------------------------- */

TB_TEST(core_atom, strings_that_land_in_the_same_slot_stay_distinct)
{
    enum
    {
        COLLIDING_COUNT = 12,
        /* Sixteen bits of the hash agree, so the strings share a slot in
         * every table of up to 65536 slots. */
        SHARED_BITS_MASK = 0xFFFF
    };
    tb_atom_table *const table = tb_atom_table_create(NULL);
    char names[COLLIDING_COUNT][24] = {{0}};
    const char *atoms[COLLIDING_COUNT] = {0};
    size_t found = {0};

    /* Search for names whose hashes end in the same sixteen bits as the
     * first one's. */
    for (size_t candidate = 0; found < COLLIDING_COUNT; candidate++)
    {
        char name[24] = {0};

        (void)snprintf(name, sizeof(name), "key_%zu", candidate);
        if ((tb_hash_string(name) & SHARED_BITS_MASK) ==
            (tb_hash_string("key_0") & SHARED_BITS_MASK))
        {
            memcpy(names[found], name, sizeof(name));
            found++;
        }
    }
    for (size_t index = 0; index < COLLIDING_COUNT; index++)
    {
        atoms[index] = tb_atom_string(table, names[index]);
        for (size_t other = 0; other < index; other++)
        {
            TB_TEST_EXPECT_POINTER(atoms[index], !=, atoms[other]);
        }
    }
    /* Each is still found, past the others in its way, in either order. */
    for (size_t index = COLLIDING_COUNT; index > 0; index--)
    {
        TB_TEST_CONTEXT("\"%s\"", names[index - 1]);
        TB_TEST_EXPECT_POINTER(tb_atom_string(table, names[index - 1]), ==, atoms[index - 1]);
        TB_TEST_EXPECT_STRING(atoms[index - 1], ==, names[index - 1]);
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, COLLIDING_COUNT);
    tb_atom_table_destroy(table);
}

TB_TEST(core_atom, a_million_atoms_survive_every_growth_of_the_table)
{
    enum
    {
        ATOM_COUNT = 1000000
    };
    /* In an arena, so that a million small allocations cost little. */
    tb_arena *const arena = tb_arena_create((size_t)4 * 1024 * 1024);
    const tb_allocator in_arena = tb_arena_allocator(arena);
    tb_atom_table *const table = tb_atom_table_create(&in_arena);
    const char **const atoms = TB_NEW_ARRAY(const char *, ATOM_COUNT);
    size_t wrong = {0};

    for (size_t index = 0; index < ATOM_COUNT; index++)
    {
        char name[24] = {0};
        const int length = snprintf(name, sizeof(name), "%zu", index * 7919);

        atoms[index] = tb_atom_new(table, name, (size_t)length);
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, ATOM_COUNT);
    /* Every atom is still where it was, with its own bytes, and interning its
     * bytes again finds it. */
    for (size_t index = 0; index < ATOM_COUNT; index++)
    {
        char name[24] = {0};
        const int length = snprintf(name, sizeof(name), "%zu", index * 7919);

        if (tb_atom_new(table, name, (size_t)length) != atoms[index] ||
            tb_atom_length(atoms[index]) != (size_t)length || strcmp(atoms[index], name) != 0)
        {
            wrong++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, ATOM_COUNT);
    tb_free(atoms);
    tb_arena_destroy(arena);
}

/* --- random interning against a model ----------------------------------------- */

enum
{
    MODEL_ENTRY_LIMIT = 6000,
    MODEL_STRING_LIMIT = 6
};

/* The reference: every distinct string seen so far, with its atom. */
typedef struct model_entry
{
    char bytes[MODEL_STRING_LIMIT];
    size_t length;
    const char *atom;
} model_entry;

TB_TEST(core_atom, random_strings_agree_with_a_list_of_everything_interned)
{
    enum
    {
        OPERATION_COUNT = 20000
    };
    /* Three letters and a zero byte: short strings repeat often. */
    static const char alphabet[] = {'a', 'b', 'c', '\0'};
    static model_entry entries[MODEL_ENTRY_LIMIT] = {0};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator in_arena = tb_arena_allocator(arena);
    /* The test allocator tracks a limited number of blocks, so it watches a
     * small table while an arena holds the large one. */
    tb_atom_table *const table = tb_atom_table_create(&in_arena);
    tb_atom_table *const watched = tb_atom_table_create(&allocator);
    size_t entry_count = {0};
    size_t repeat_count = {0};

    memset(entries, 0, sizeof(entries));
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        char bytes[MODEL_STRING_LIMIT] = {0};
        const size_t length = (size_t)tb_test_random_below(MODEL_STRING_LIMIT + 1);
        const char *atom = NULL;
        size_t found = entry_count;

        for (size_t index = 0; index < length; index++)
        {
            bytes[index] = alphabet[tb_test_random_below(sizeof(alphabet))];
        }
        for (size_t index = 0; index < entry_count; index++)
        {
            if (entries[index].length == length && memcmp(entries[index].bytes, bytes, length) == 0)
            {
                found = index;
                break;
            }
        }
        TB_TEST_CONTEXT("operation %zu, a string of %zu bytes", operation, length);
        if (found < entry_count)
        {
            /* Seen before: found without being created, and interned to the
             * same pointer. */
            TB_TEST_EXPECT_POINTER(tb_atom_find(table, bytes, length), ==, entries[found].atom);
            TB_TEST_EXPECT_POINTER(tb_atom_new(table, bytes, length), ==, entries[found].atom);
            repeat_count++;
            continue;
        }
        TB_TEST_REQUIRE_UINT(entry_count, <, MODEL_ENTRY_LIMIT);
        TB_TEST_EXPECT_POINTER(tb_atom_find(table, bytes, length), ==, NULL);
        atom = tb_atom_new(table, bytes, length);
        expect_atom(atom, bytes, length);
        memcpy(entries[entry_count].bytes, bytes, length);
        entries[entry_count].length = length;
        entries[entry_count].atom = atom;
        entry_count++;
        TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, entry_count);
        if (entry_count <= 200)
        {
            (void)tb_atom_new(watched, bytes, length);
        }
    }
    TB_TEST_EXPECT_UINT(repeat_count, >, OPERATION_COUNT / 2);
    TB_TEST_EXPECT_UINT(entry_count, >, 1000);
    /* Nothing that was interned along the way moved or changed. */
    for (size_t index = 0; index < entry_count; index++)
    {
        expect_atom(entries[index].atom, entries[index].bytes, entries[index].length);
    }
    TB_TEST_EXPECT_UINT(tb_atom_table_count(watched), ==, 200);
    tb_atom_table_destroy(watched);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    tb_arena_destroy(arena);
}

/* --- memory ------------------------------------------------------------------- */

/* The sizes below follow from how atoms are laid out. An atom takes a header
 * of 16 bytes, its bytes, and a terminator, rounded up to a multiple of 8: the
 * decimal digits of a small number take 24 bytes. A block starts with 16 bytes
 * of its own. The first block is 1,024 bytes and each one after it is twice
 * the size of the last, up to 65,536. */
enum
{
    SMALL_ATOM_SIZE = 24,
    BLOCK_HEADER_SIZE = 16
};

TB_TEST(core_atom, all_memory_comes_from_the_allocator_and_goes_back_to_it)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);

    for (int value = 0; value < 500; value++)
    {
        const char *const atom = tb_atom_int(table, value);

        TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, atom, tb_atom_length(atom) + 1));
    }
    /* The table, its slots, and four blocks: blocks of 1,024, 2,048, 4,096,
     * and 8,192 bytes hold 42, 84, 170, and 340 of these atoms. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 6);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_atom, the_table_doubles_when_it_would_pass_three_quarters_full)
{
    /* The atoms that make the table take new slots: the first, and then each
     * one that would leave it more than three quarters full, which is the
     * thirteenth of 16 slots, the twenty-fifth of 32, and so on. Growing later
     * makes every search longer; growing sooner or by more wastes memory. */
    static const size_t growing_atoms[] = {1, 13, 25, 49, 97, 193};
    /* The atoms that need a new block: the first, and then the one after
     * each block is full, with 42 in the first block and 84 in the second. */
    static const size_t block_atoms[] = {1, 43, 127};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);
    size_t next_growth = {0};
    size_t next_block = {0};

    for (size_t number = 1; number <= 200; number++)
    {
        const size_t allocations_before = tracker.allocation_count;
        size_t expected_allocations = 0;

        (void)tb_atom_int(table, (long long)number);
        if (next_growth < TB_TEST_COUNT_OF(growing_atoms) && number == growing_atoms[next_growth])
        {
            expected_allocations++;
            next_growth++;
        }
        if (next_block < TB_TEST_COUNT_OF(block_atoms) && number == block_atoms[next_block])
        {
            expected_allocations++;
            next_block++;
        }
        TB_TEST_CONTEXT("atom %zu", number);
        TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations_before, ==,
                            expected_allocations);
    }
    /* The slots that were outgrown went back: the table, its slots, and three
     * blocks of atoms are all that is left. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 5);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* A table of many atoms must take few blocks, and a table of few atoms a
 * small one. Blocks that did not grow would be 239 for these atoms; blocks
 * that grew without limit would be 8. */
TB_TEST(core_atom, blocks_double_in_size_up_to_a_limit)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);

    (void)tb_atom_int(table, 0);
    /* The table, its first slots, and a first block of 1,024 bytes. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 3);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, <, 2048);
    for (int value = 1; value < 10000; value++)
    {
        (void)tb_atom_int(table, value);
    }
    /* Seven blocks from 1,024 to 65,536 bytes hold the first 5,412 of these
     * atoms, and two more of 65,536 the rest: nine, with the table and its
     * slots. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 11);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* A name much longer than the others, long enough to need memory of its own
 * wherever the table has got to. */
enum
{
    LONG_NAME_LENGTH = 3000
};

static const char *long_name(void)
{
    static char name[LONG_NAME_LENGTH + 1] = {0};

    memset(name, 'n', LONG_NAME_LENGTH);
    return name;
}

TB_TEST(core_atom, a_long_atom_gets_a_block_of_its_own_and_the_shared_block_carries_on)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);
    char medium[500] = {0};
    const char *first = NULL;
    const char *second = NULL;
    const char *third = NULL;
    const char *fourth = NULL;
    const char *long_atom = NULL;
    size_t allocations = {0};
    size_t bytes = {0};

    memset(medium, 'm', sizeof(medium));
    first = tb_atom_string(table, "a");
    allocations = tracker.allocation_count;
    bytes = tracker.live_byte_count;

    /* The long atom does not fit in what is left of the first block and is
     * more than a quarter of the next one, so it gets a block of exactly its
     * size: 16 bytes of block, 16 of header, 3,001 of name rounded up to 3,008. */
    long_atom = tb_atom_new(table, long_name(), LONG_NAME_LENGTH);
    expect_atom(long_atom, long_name(), LONG_NAME_LENGTH);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count - bytes, ==, BLOCK_HEADER_SIZE + 16 + 3008);

    /* The first block was not given up for it: the next atoms follow the
     * first one there, each rounded up to a multiple of 8 bytes. */
    second = tb_atom_string(table, "b");
    TB_TEST_EXPECT_POINTER(second, ==, first + SMALL_ATOM_SIZE);
    third = tb_atom_new(table, medium, 400);
    TB_TEST_EXPECT_POINTER(third, ==, second + SMALL_ATOM_SIZE);
    fourth = tb_atom_new(table, medium, 500);
    TB_TEST_EXPECT_POINTER(fourth, ==, third + 16 + 408);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 1);

    /* Sixteen bytes are left in the first block. An atom that does not fit
     * and is no more than a quarter of the next block starts that block,
     * which is twice the size of the first, and the atom after it follows. */
    bytes = tracker.live_byte_count;
    first = tb_atom_string(table, "c");
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count - bytes, ==, 2048);
    second = tb_atom_string(table, "d");
    TB_TEST_EXPECT_POINTER(second, ==, first + SMALL_ATOM_SIZE);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 2);

    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 7);
    expect_atom(long_atom, long_name(), LONG_NAME_LENGTH);
    expect_atom(third, medium, 400);
    expect_atom(fourth, medium, 500);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* Where the line falls between an atom that shares a block and one that gets
 * its own. An atom of `length` bytes takes `length + 17` bytes rounded up to
 * a multiple of 8. */
TB_TEST(core_atom, an_atom_gets_its_own_block_only_past_a_quarter_of_the_next_one)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);
    char bytes[1600] = {0};
    const char *first = NULL;
    const char *exact = NULL;
    size_t allocations = {0};
    size_t live_bytes = {0};

    memset(bytes, 'q', sizeof(bytes));
    first = tb_atom_string(table, "a");
    allocations = tracker.allocation_count;

    /* 984 bytes are left in the first block. An atom of exactly that size
     * fits, however large a part of the next block it would be. */
    exact = tb_atom_new(table, bytes, 984 - 17);
    TB_TEST_EXPECT_POINTER(exact, ==, first + SMALL_ATOM_SIZE);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 0);

    /* The first block is full and the next will be 2,048 bytes. An atom of
     * exactly a quarter of that, 512 bytes, starts it. */
    live_bytes = tracker.live_byte_count;
    (void)tb_atom_new(table, bytes, 512 - 17);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count - live_bytes, ==, 2048);

    /* 1,520 bytes are left in that block and the next will be 4,096. An atom
     * of 1,600 does not fit and is more than a quarter of 4,096, so it gets a
     * block of its own, of its size and the 16 bytes every block starts with. */
    live_bytes = tracker.live_byte_count;
    (void)tb_atom_new(table, bytes, 1600 - 17);
    TB_TEST_EXPECT_UINT(tracker.allocation_count - allocations, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count - live_bytes, ==, BLOCK_HEADER_SIZE + 1600);

    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 4);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

static void intern_the_long_name(void *context)
{
    (void)tb_atom_new(context, long_name(), LONG_NAME_LENGTH);
}

static void create_a_table(void *context)
{
    (void)tb_atom_table_create(context);
}

TB_TEST(core_atom, an_exhausted_allocator_ends_interning_and_leaves_the_table_intact)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);
    const char *atoms[40] = {0};
    size_t live_blocks = {0};

    for (int value = 0; value < 40; value++)
    {
        atoms[value] = tb_atom_int(table, value);
    }
    live_blocks = tracker.live_block_count;
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(intern_the_long_name, table, "out of memory");
    TB_TEST_EXPECT_TRAP(create_a_table, &allocator, "out of memory");
    /* An atom that exists needs no memory to be found. */
    TB_TEST_EXPECT_POINTER(tb_atom_int(table, 7), ==, atoms[7]);
    tb_test_allocator_stop_failing(&tracker);

    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 40);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, live_blocks);
    for (int value = 0; value < 40; value++)
    {
        TB_TEST_EXPECT_POINTER(tb_atom_int(table, value), ==, atoms[value]);
    }
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, long_name(), LONG_NAME_LENGTH), ==, NULL);
    expect_atom(tb_atom_new(table, long_name(), LONG_NAME_LENGTH), long_name(), LONG_NAME_LENGTH);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_atom, running_out_of_memory_after_the_table_grew_leaves_no_atom_behind)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_atom_table *const table = tb_atom_table_create(&allocator);
    const char *atoms[12] = {0};
    size_t live_blocks = {0};

    /* Twelve atoms fill the first slots, so the long one after them needs two
     * allocations: larger slots, and a block for itself. */
    for (int value = 0; value < 12; value++)
    {
        atoms[value] = tb_atom_int(table, value);
    }
    live_blocks = tracker.live_block_count;
    /* The first of the two is supplied and the second refused. The table
     * must have asked for the slots first: they replace the old ones, where
     * a block for the atom allocated first would hold an atom that no slot
     * refers to. */
    tb_test_allocator_fail_after(&tracker, 1);
    TB_TEST_EXPECT_TRAP(intern_the_long_name, table, "out of memory");
    tb_test_allocator_stop_failing(&tracker);

    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, live_blocks);
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 12);
    for (int value = 0; value < 12; value++)
    {
        TB_TEST_EXPECT_POINTER(tb_atom_int(table, value), ==, atoms[value]);
    }
    TB_TEST_EXPECT_POINTER(tb_atom_find(table, long_name(), LONG_NAME_LENGTH), ==, NULL);
    expect_atom(tb_atom_new(table, long_name(), LONG_NAME_LENGTH), long_name(), LONG_NAME_LENGTH);
    tb_atom_table_destroy(table);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

static void count_of_null(void *context)
{
    (void)context;
    (void)tb_atom_table_count(NULL);
}

static void new_in_null(void *context)
{
    (void)context;
    (void)tb_atom_new(NULL, "a", 1);
}

static void string_in_null(void *context)
{
    (void)context;
    (void)tb_atom_string(NULL, "a");
}

static void int_in_null(void *context)
{
    (void)context;
    (void)tb_atom_int(NULL, 1);
}

static void find_in_null(void *context)
{
    (void)context;
    (void)tb_atom_find(NULL, "a", 1);
}

static void new_without_bytes(void *context)
{
    (void)tb_atom_new(context, NULL, 3);
}

static void find_without_bytes(void *context)
{
    (void)tb_atom_find(context, NULL, 3);
}

static void string_without_a_string(void *context)
{
    (void)tb_atom_string(context, NULL);
}

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_atom_length(NULL);
}

static void hash_of_null(void *context)
{
    (void)context;
    (void)tb_atom_hash(NULL);
}

TB_TEST(core_atom, missing_tables_and_bytes_violate_the_contract)
{
    tb_atom_table *table = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    table = tb_atom_table_create(NULL);
    TB_TEST_EXPECT_TRAP(count_of_null, NULL, "table != NULL");
    TB_TEST_EXPECT_TRAP(new_in_null, NULL, "table != NULL");
    TB_TEST_EXPECT_TRAP(string_in_null, NULL, "table != NULL");
    TB_TEST_EXPECT_TRAP(int_in_null, NULL, "table != NULL");
    TB_TEST_EXPECT_TRAP(find_in_null, NULL, "table != NULL");
    TB_TEST_EXPECT_TRAP(new_without_bytes, table, "bytes != NULL || length == 0");
    TB_TEST_EXPECT_TRAP(find_without_bytes, table, "bytes != NULL || length == 0");
    TB_TEST_EXPECT_TRAP(string_without_a_string, table, "string != NULL");
    TB_TEST_EXPECT_TRAP(length_of_null, NULL, "atom != NULL");
    TB_TEST_EXPECT_TRAP(hash_of_null, NULL, "atom != NULL");
    TB_TEST_EXPECT_UINT(tb_atom_table_count(table), ==, 0);
    tb_atom_table_destroy(table);
}
