/* A bit vector packs its bits into words, so its mistakes hide at word
 * boundaries and in the unused positions of the last word. Every operation is
 * therefore compared with a model that keeps one bool per bit, on lengths that
 * end before, on, and after a word boundary. */
#include "badger/bit.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

enum
{
    MODEL_LENGTH_LIMIT = 300
};

/* The reference: one bool per bit. */
typedef struct bit_model
{
    bool values[MODEL_LENGTH_LIMIT];
    size_t length;
} bit_model;

/* Lengths that end before, on, and after the boundaries of 64-bit words. */
static const size_t lengths[] = {0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 193, 300};

/* Fills a model with random bits. One time in four they are sparse and one
 * time in four dense, so that long runs of equal bits occur. */
static void randomize_model(bit_model *model, size_t length)
{
    const uint64_t density = tb_test_random_below(4);

    model->length = length;
    for (size_t index = 0; index < length; index++)
    {
        switch (density)
        {
        case 0:
            model->values[index] = tb_test_random_below(20) == 0;
            break;
        case 1:
            model->values[index] = tb_test_random_below(20) != 0;
            break;
        default:
            model->values[index] = tb_test_random_below(2) == 0;
            break;
        }
    }
}

static tb_bit *create_from_model(const tb_allocator *allocator, const bit_model *model)
{
    tb_bit *const bits = tb_bit_create(allocator, model->length);

    for (size_t index = 0; index < model->length; index++)
    {
        (void)tb_bit_put(bits, index, model->values[index]);
    }
    return bits;
}

static size_t model_count(const bit_model *model)
{
    size_t count = {0};

    for (size_t index = 0; index < model->length; index++)
    {
        if (model->values[index])
        {
            count++;
        }
    }
    return count;
}

/* Number of bits in which a vector differs from a model. */
static size_t count_differences(const tb_bit *bits, const bit_model *model)
{
    size_t differences = {0};

    if (tb_bit_length(bits) != model->length)
    {
        return SIZE_MAX;
    }
    for (size_t index = 0; index < model->length; index++)
    {
        if (tb_bit_get(bits, index) != model->values[index])
        {
            differences++;
        }
    }
    return differences;
}

/* Checks a vector against a model, including the count, which also proves
 * that no bit beyond the length is set. */
static void expect_model(const tb_bit *bits, const bit_model *model)
{
    TB_TEST_EXPECT_UINT(count_differences(bits, model), ==, 0);
    TB_TEST_EXPECT_UINT(tb_bit_count(bits), ==, model_count(model));
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_bit, a_new_vector_has_every_bit_clear)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        tb_test_allocator tracker = {0};
        const tb_allocator allocator = tb_test_allocator_start(&tracker);
        tb_bit *const bits = tb_bit_create(&allocator, lengths[row]);

        TB_TEST_CONTEXT("length %zu", lengths[row]);
        TB_TEST_REQUIRE_POINTER(bits, !=, NULL);
        TB_TEST_EXPECT_UINT(tb_bit_length(bits), ==, lengths[row]);
        TB_TEST_EXPECT_UINT(tb_bit_count(bits), ==, 0);
        for (size_t index = 0; index < lengths[row]; index++)
        {
            TB_TEST_EXPECT(!tb_bit_get(bits, index));
        }
        TB_TEST_EXPECT_UINT(tb_bit_find_set(bits, 0), ==, TB_BIT_NOT_FOUND);
        /* One block holds the vector, and it is no larger than its bits need,
         * after a header of a few words. */
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
        TB_TEST_EXPECT_UINT(tracker.live_byte_count, <=, lengths[row] / 8 + 80);
        tb_bit_destroy(bits);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
        TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    }
}

/* Bytes a vector of a length takes from its allocator. */
static size_t bytes_for_length(size_t length)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_bit *const bits = tb_bit_create(&allocator, length);
    const size_t bytes = tracker.live_byte_count;

    tb_bit_destroy(bits);
    return bytes;
}

TB_TEST(core_bit, a_vector_takes_one_word_for_every_64_bits_or_part_of_them)
{
    const size_t empty = bytes_for_length(0);

    TB_TEST_EXPECT_UINT(bytes_for_length(1), ==, empty + 8);
    TB_TEST_EXPECT_UINT(bytes_for_length(64), ==, empty + 8);
    TB_TEST_EXPECT_UINT(bytes_for_length(65), ==, empty + 16);
    TB_TEST_EXPECT_UINT(bytes_for_length(128), ==, empty + 16);
    TB_TEST_EXPECT_UINT(bytes_for_length(129), ==, empty + 24);
    TB_TEST_EXPECT_UINT(bytes_for_length(64000), ==, empty + 8000);
}

TB_TEST(core_bit, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_bit_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_bit, copy_makes_an_independent_vector_in_any_allocator)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        tb_test_allocator tracker = {0};
        const tb_allocator allocator = tb_test_allocator_start(&tracker);
        bit_model model = {0};
        tb_bit *original = NULL;
        tb_bit *copy = NULL;

        TB_TEST_CONTEXT("length %zu", lengths[row]);
        randomize_model(&model, lengths[row]);
        original = create_from_model(NULL, &model);
        copy = tb_bit_copy(original, &allocator);
        TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, copy, 1));
        expect_model(copy, &model);
        TB_TEST_EXPECT(tb_bit_equal(copy, original));
        /* Changing the original does not change the copy. */
        tb_bit_flip(original, 0, lengths[row]);
        expect_model(copy, &model);
        tb_bit_destroy(copy);
        tb_bit_destroy(original);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    }
}

TB_TEST(core_bit, a_vector_in_an_arena_needs_no_destroy)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    const size_t blocks = tb_memory_allocation_count();
    tb_bit *const bits = tb_bit_create(&allocator, 10000);

    tb_bit_set(bits, 100, 9000);
    TB_TEST_EXPECT_UINT(tb_bit_count(bits), ==, 8900);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    tb_arena_destroy(arena);
}

/* --- single bits -------------------------------------------------------------- */

TB_TEST(core_bit, put_changes_one_bit_and_returns_what_it_was)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        tb_bit *const bits = tb_bit_create(NULL, lengths[row]);
        bit_model model = {.length = lengths[row]};

        TB_TEST_CONTEXT("length %zu", lengths[row]);
        /* Set every third bit, one at a time, and check all bits each time. */
        for (size_t index = 0; index < lengths[row]; index += 3)
        {
            TB_TEST_EXPECT(!tb_bit_put(bits, index, true));
            model.values[index] = true;
            TB_TEST_EXPECT_UINT(count_differences(bits, &model), ==, 0);
            /* Writing the value a bit already has reports it and changes nothing. */
            TB_TEST_EXPECT(tb_bit_put(bits, index, true));
        }
        expect_model(bits, &model);
        for (size_t index = 0; index < lengths[row]; index++)
        {
            TB_TEST_EXPECT(tb_bit_put(bits, index, false) == model.values[index]);
            model.values[index] = false;
            TB_TEST_EXPECT(!tb_bit_put(bits, index, false));
        }
        expect_model(bits, &model);
        tb_bit_destroy(bits);
    }
}

/* --- ranges ------------------------------------------------------------------- */

typedef void range_function(tb_bit *bits, size_t begin, size_t end);

/* Applies a range function to every range of a vector and compares the result
 * with the model after each. */
static void check_every_range(range_function *function, bool sets, bool clears)
{
    enum
    {
        /* Two full words and a part of a third. */
        LENGTH = 150
    };
    bit_model base = {0};
    tb_bit *original = NULL;

    randomize_model(&base, LENGTH);
    original = create_from_model(NULL, &base);
    for (size_t begin = 0; begin <= LENGTH; begin++)
    {
        for (size_t end = begin; end <= LENGTH; end++)
        {
            tb_bit *const bits = tb_bit_copy(original, NULL);
            bit_model model = base;

            for (size_t index = begin; index < end; index++)
            {
                if (sets)
                {
                    model.values[index] = true;
                }
                else if (clears)
                {
                    model.values[index] = false;
                }
                else
                {
                    model.values[index] = !model.values[index];
                }
            }
            function(bits, begin, end);
            if (count_differences(bits, &model) != 0 || tb_bit_count(bits) != model_count(&model))
            {
                TB_TEST_FAIL("the range from %zu to %zu left %zu bits wrong", begin, end,
                             count_differences(bits, &model));
            }
            tb_bit_destroy(bits);
        }
    }
    tb_bit_destroy(original);
}

TB_TEST(core_bit, set_sets_exactly_the_bits_of_a_range)
{
    check_every_range(tb_bit_set, true, false);
}

TB_TEST(core_bit, clear_clears_exactly_the_bits_of_a_range)
{
    check_every_range(tb_bit_clear, false, true);
}

TB_TEST(core_bit, flip_inverts_exactly_the_bits_of_a_range)
{
    check_every_range(tb_bit_flip, false, false);
}

TB_TEST(core_bit, whole_vector_ranges_leave_nothing_beyond_the_length)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t length = lengths[row];
        tb_bit *const bits = tb_bit_create(NULL, length);
        tb_bit *const flipped = tb_bit_create(NULL, length);

        TB_TEST_CONTEXT("length %zu", length);
        tb_bit_set(bits, 0, length);
        tb_bit_flip(flipped, 0, length);
        /* A stray bit beyond the length would show in the count, would make
         * two full vectors differ, and would be found as a clear bit. */
        TB_TEST_EXPECT_UINT(tb_bit_count(bits), ==, length);
        TB_TEST_EXPECT_UINT(tb_bit_count(flipped), ==, length);
        TB_TEST_EXPECT(tb_bit_equal(bits, flipped));
        TB_TEST_EXPECT_UINT(tb_bit_find_clear(bits, 0), ==, TB_BIT_NOT_FOUND);
        tb_bit_flip(flipped, 0, length);
        TB_TEST_EXPECT_UINT(tb_bit_count(flipped), ==, 0);
        tb_bit_clear(bits, 0, length);
        TB_TEST_EXPECT(tb_bit_equal(bits, flipped));
        tb_bit_destroy(flipped);
        tb_bit_destroy(bits);
    }
}

/* --- searching ---------------------------------------------------------------- */

/* The first index at or after `from` whose bit has the wanted value. */
static size_t model_find(const bit_model *model, size_t from, bool wanted)
{
    for (size_t index = from; index < model->length; index++)
    {
        if (model->values[index] == wanted)
        {
            return index;
        }
    }
    return TB_BIT_NOT_FOUND;
}

TB_TEST(core_bit, find_returns_the_first_matching_bit_from_every_position)
{
    enum
    {
        ROUND_COUNT = 40
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        for (size_t round = 0; round < ROUND_COUNT; round++)
        {
            bit_model model = {0};
            tb_bit *bits = NULL;

            randomize_model(&model, lengths[row]);
            bits = create_from_model(NULL, &model);
            for (size_t from = 0; from <= lengths[row]; from++)
            {
                TB_TEST_CONTEXT("length %zu, round %zu, from %zu", lengths[row], round, from);
                TB_TEST_EXPECT_UINT(tb_bit_find_set(bits, from), ==,
                                    model_find(&model, from, true));
                TB_TEST_EXPECT_UINT(tb_bit_find_clear(bits, from), ==,
                                    model_find(&model, from, false));
            }
            tb_bit_destroy(bits);
        }
    }
}

TB_TEST(core_bit, a_search_loop_visits_every_set_bit_once)
{
    bit_model model = {0};
    tb_bit *bits = NULL;
    size_t visited = {0};
    size_t previous = {0};

    randomize_model(&model, MODEL_LENGTH_LIMIT);
    model.values[0] = true;
    model.values[MODEL_LENGTH_LIMIT - 1] = true;
    bits = create_from_model(NULL, &model);
    for (size_t index = tb_bit_find_set(bits, 0); index != TB_BIT_NOT_FOUND;
         index = tb_bit_find_set(bits, index + 1))
    {
        TB_TEST_REQUIRE_UINT(index, <, MODEL_LENGTH_LIMIT);
        TB_TEST_EXPECT(model.values[index]);
        TB_TEST_EXPECT(visited == 0 || index > previous);
        previous = index;
        visited++;
    }
    TB_TEST_EXPECT_UINT(visited, ==, model_count(&model));
    tb_bit_destroy(bits);
}

/* Walks a vector with a cursor and checks that it returns exactly the set
 * bits of the model, lowest first. */
static void expect_cursor_walk(const tb_bit *bits, const bit_model *model)
{
    tb_bit_cursor cursor = {0};
    size_t index = SIZE_MAX;
    size_t expected = {0};
    size_t visited = {0};

    while (tb_bit_next_set(bits, &cursor, &index))
    {
        /* The next bit of the model that is set is the one returned. */
        while (expected < model->length && !model->values[expected])
        {
            expected++;
        }
        TB_TEST_REQUIRE_UINT(index, ==, expected);
        expected++;
        visited++;
    }
    TB_TEST_EXPECT_UINT(visited, ==, model_count(model));
    /* A walk that has ended stays ended, and leaves the index alone. */
    index = SIZE_MAX;
    TB_TEST_EXPECT(!tb_bit_next_set(bits, &cursor, &index));
    TB_TEST_EXPECT(!tb_bit_next_set(bits, &cursor, &index));
    TB_TEST_EXPECT_UINT(index, ==, SIZE_MAX);
}

TB_TEST(core_bit, a_cursor_steps_through_exactly_the_set_bits_in_order)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t length = lengths[row];
        bit_model model = {.length = length};
        tb_bit *bits = create_from_model(NULL, &model);

        /* No bit set: the walk is over before it starts. */
        TB_TEST_CONTEXT("%zu bits, all clear", length);
        expect_cursor_walk(bits, &model);

        /* Every bit set: each word gives up all of its bits, and the last
         * word none beyond the length. */
        TB_TEST_CONTEXT("%zu bits, all set", length);
        tb_bit_set(bits, 0, length);
        for (size_t index = 0; index < length; index++)
        {
            model.values[index] = true;
        }
        expect_cursor_walk(bits, &model);
        tb_bit_destroy(bits);

        /* One bit, at each end and beside each boundary between words. */
        for (size_t place = 0; place < length; place++)
        {
            if (place % 64 > 1 && place % 64 < 62 && place + 1 != length)
            {
                continue;
            }
            TB_TEST_CONTEXT("%zu bits, only bit %zu set", length, place);
            model = (bit_model){.length = length};
            model.values[place] = true;
            bits = create_from_model(NULL, &model);
            expect_cursor_walk(bits, &model);
            tb_bit_destroy(bits);
        }

        for (size_t round = 0; round < 20; round++)
        {
            TB_TEST_CONTEXT("%zu bits, random round %zu", length, round);
            randomize_model(&model, length);
            bits = create_from_model(NULL, &model);
            expect_cursor_walk(bits, &model);
            tb_bit_destroy(bits);
        }
    }
}

/* The two ways to visit the set bits return the same bits. */
TB_TEST(core_bit, a_cursor_and_a_search_loop_visit_the_same_bits)
{
    bit_model model = {0};
    tb_bit *bits = NULL;
    tb_bit_cursor cursor = {0};
    size_t stepped = {0};

    randomize_model(&model, MODEL_LENGTH_LIMIT);
    bits = create_from_model(NULL, &model);
    for (size_t searched = tb_bit_find_set(bits, 0); searched != TB_BIT_NOT_FOUND;
         searched = tb_bit_find_set(bits, searched + 1))
    {
        TB_TEST_REQUIRE(tb_bit_next_set(bits, &cursor, &stepped));
        TB_TEST_EXPECT_UINT(stepped, ==, searched);
    }
    TB_TEST_EXPECT(!tb_bit_next_set(bits, &cursor, &stepped));
    tb_bit_destroy(bits);
}

typedef struct visit_record
{
    tb_bit *bits;
    size_t call_count;
    size_t out_of_order_count;
    size_t set_count;
} visit_record;

static void record_visit(size_t index, bool value, void *context)
{
    visit_record *const record = context;

    if (index != record->call_count)
    {
        record->out_of_order_count++;
    }
    record->call_count++;
    if (value)
    {
        record->set_count++;
    }
    /* Setting the next bit must be seen by the visit that follows. */
    if (index == 10)
    {
        (void)tb_bit_put(record->bits, 11, true);
    }
}

TB_TEST(core_bit, visit_calls_the_visitor_for_every_bit_in_order)
{
    tb_bit *const bits = tb_bit_create(NULL, 200);
    visit_record record = {.bits = bits};

    tb_bit_set(bits, 64, 128);
    tb_bit_visit(bits, record_visit, &record);
    TB_TEST_EXPECT_UINT(record.call_count, ==, 200);
    TB_TEST_EXPECT_UINT(record.out_of_order_count, ==, 0);
    /* The 64 bits that were set, and the one the visitor set on the way. */
    TB_TEST_EXPECT_UINT(record.set_count, ==, 65);
    tb_bit_destroy(bits);
}

/* --- two vectors -------------------------------------------------------------- */

typedef void combine_function(tb_bit *bits, const tb_bit *other);

typedef enum combination
{
    COMBINATION_UNION,
    COMBINATION_INTERSECTION,
    COMBINATION_DIFFERENCE,
    COMBINATION_SYMMETRIC_DIFFERENCE
} combination;

static bool combine_values(combination kind, bool left, bool right)
{
    switch (kind)
    {
    case COMBINATION_UNION:
        return left || right;
    case COMBINATION_INTERSECTION:
        return left && right;
    case COMBINATION_DIFFERENCE:
        return left && !right;
    case COMBINATION_SYMMETRIC_DIFFERENCE:
        return left != right;
    }
    return false;
}

static void check_combination(combine_function *function, combination kind)
{
    enum
    {
        ROUND_COUNT = 40
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        for (size_t round = 0; round < ROUND_COUNT; round++)
        {
            bit_model left_model = {0};
            bit_model right_model = {0};
            bit_model expected = {0};
            bit_model with_itself = {0};
            tb_bit *left = NULL;
            tb_bit *right = NULL;

            randomize_model(&left_model, lengths[row]);
            randomize_model(&right_model, lengths[row]);
            expected.length = lengths[row];
            with_itself.length = lengths[row];
            for (size_t index = 0; index < lengths[row]; index++)
            {
                expected.values[index] =
                    combine_values(kind, left_model.values[index], right_model.values[index]);
                with_itself.values[index] =
                    combine_values(kind, right_model.values[index], right_model.values[index]);
            }
            left = create_from_model(NULL, &left_model);
            right = create_from_model(NULL, &right_model);
            TB_TEST_CONTEXT("length %zu, round %zu", lengths[row], round);
            function(left, right);
            expect_model(left, &expected);
            /* The other vector is only read, and may be the vector itself. */
            function(right, right);
            expect_model(right, &with_itself);
            tb_bit_destroy(right);
            tb_bit_destroy(left);
        }
    }
}

TB_TEST(core_bit, union_sets_the_bits_of_either_vector)
{
    check_combination(tb_bit_union, COMBINATION_UNION);
}

TB_TEST(core_bit, intersection_keeps_the_bits_of_both_vectors)
{
    check_combination(tb_bit_intersection, COMBINATION_INTERSECTION);
}

TB_TEST(core_bit, difference_clears_the_bits_of_the_other_vector)
{
    check_combination(tb_bit_difference, COMBINATION_DIFFERENCE);
}

TB_TEST(core_bit, symmetric_difference_keeps_the_bits_of_exactly_one_vector)
{
    check_combination(tb_bit_symmetric_difference, COMBINATION_SYMMETRIC_DIFFERENCE);
}

TB_TEST(core_bit, comparisons_agree_with_the_model_on_related_vectors)
{
    enum
    {
        ROUND_COUNT = 60
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        for (size_t round = 0; round < ROUND_COUNT; round++)
        {
            const size_t length = lengths[row];
            bit_model left_model = {0};
            bit_model right_model = {0};
            tb_bit *left = NULL;
            tb_bit *right = NULL;
            bool is_equal = true;
            bool is_subset = true;

            /* Independent vectors are almost never subsets, so most rounds
             * derive the right vector from the left one: a copy, a copy with
             * a bit added, or a copy with a bit removed. */
            randomize_model(&left_model, length);
            right_model = left_model;
            if (round % 4 == 0)
            {
                randomize_model(&right_model, length);
            }
            if (round % 4 >= 2 && length > 0)
            {
                const size_t changed = (size_t)tb_test_random_below(length);

                right_model.values[changed] = round % 4 == 2;
            }
            for (size_t index = 0; index < length; index++)
            {
                if (left_model.values[index] != right_model.values[index])
                {
                    is_equal = false;
                }
                if (left_model.values[index] && !right_model.values[index])
                {
                    is_subset = false;
                }
            }
            left = create_from_model(NULL, &left_model);
            right = create_from_model(NULL, &right_model);
            TB_TEST_CONTEXT("length %zu, round %zu", length, round);
            TB_TEST_EXPECT(tb_bit_equal(left, right) == is_equal);
            TB_TEST_EXPECT(tb_bit_equal(right, left) == is_equal);
            TB_TEST_EXPECT(tb_bit_is_subset(left, right) == is_subset);
            TB_TEST_EXPECT(tb_bit_is_proper_subset(left, right) == (is_subset && !is_equal));
            /* Every vector equals itself and is its own subset, but not a
             * proper one. */
            TB_TEST_EXPECT(tb_bit_equal(left, left));
            TB_TEST_EXPECT(tb_bit_is_subset(left, left));
            TB_TEST_EXPECT(!tb_bit_is_proper_subset(left, left));
            tb_bit_destroy(right);
            tb_bit_destroy(left);
        }
    }
}

/* --- random operations against a model ---------------------------------------- */

TB_TEST(core_bit, random_operations_agree_with_one_bool_per_bit)
{
    enum
    {
        OPERATION_COUNT = 20000,
        LENGTH = 200
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    bit_model model = {.length = LENGTH};
    bit_model other_model = {0};
    tb_bit *const bits = tb_bit_create(&allocator, LENGTH);
    tb_bit *other = NULL;

    randomize_model(&other_model, LENGTH);
    other = create_from_model(&allocator, &other_model);
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(8);
        const size_t first = (size_t)tb_test_random_below(LENGTH + 1);
        const size_t second = (size_t)tb_test_random_below(LENGTH + 1);
        const size_t begin = first < second ? first : second;
        const size_t end = first < second ? second : first;
        const size_t index = (size_t)tb_test_random_below(LENGTH);
        const bool value = tb_test_random_below(2) == 0;
        combination kind = COMBINATION_UNION;

        TB_TEST_CONTEXT("operation %zu (choice %u), range %zu to %zu, index %zu", operation,
                        (unsigned)choice, begin, end, index);
        switch (choice)
        {
        case 0:
            TB_TEST_EXPECT(tb_bit_put(bits, index, value) == model.values[index]);
            model.values[index] = value;
            break;
        case 1:
            tb_bit_set(bits, begin, end);
            memset(&model.values[begin], 1, (end - begin) * sizeof(bool));
            break;
        case 2:
            tb_bit_clear(bits, begin, end);
            memset(&model.values[begin], 0, (end - begin) * sizeof(bool));
            break;
        case 3:
            tb_bit_flip(bits, begin, end);
            for (size_t flipped = begin; flipped < end; flipped++)
            {
                model.values[flipped] = !model.values[flipped];
            }
            break;
        default:
            kind = (combination)(choice - 4);
            if (kind == COMBINATION_UNION)
            {
                tb_bit_union(bits, other);
            }
            else if (kind == COMBINATION_INTERSECTION)
            {
                tb_bit_intersection(bits, other);
            }
            else if (kind == COMBINATION_DIFFERENCE)
            {
                tb_bit_difference(bits, other);
            }
            else
            {
                tb_bit_symmetric_difference(bits, other);
            }
            for (size_t combined = 0; combined < LENGTH; combined++)
            {
                model.values[combined] =
                    combine_values(kind, model.values[combined], other_model.values[combined]);
            }
            break;
        }
        expect_model(bits, &model);
        TB_TEST_EXPECT_UINT(tb_bit_find_set(bits, index), ==, model_find(&model, index, true));
        TB_TEST_EXPECT_UINT(tb_bit_find_clear(bits, index), ==, model_find(&model, index, false));
    }
    tb_bit_destroy(other);
    tb_bit_destroy(bits);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- running out of memory ---------------------------------------------------- */

static void create_a_vector(void *context)
{
    (void)tb_bit_create(context, 1000);
}

TB_TEST(core_bit, an_exhausted_allocator_ends_creation)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);

    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(create_a_vector, &allocator, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

static void ignore_visit(size_t index, bool value, void *context)
{
    (void)index;
    (void)value;
    (void)context;
}

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_bit_length(NULL);
}

static void count_of_null(void *context)
{
    (void)context;
    (void)tb_bit_count(NULL);
}

static void get_from_null(void *context)
{
    (void)context;
    (void)tb_bit_get(NULL, 0);
}

static void put_into_null(void *context)
{
    (void)context;
    (void)tb_bit_put(NULL, 0, true);
}

static void set_in_null(void *context)
{
    (void)context;
    tb_bit_set(NULL, 0, 0);
}

static void clear_in_null(void *context)
{
    (void)context;
    tb_bit_clear(NULL, 0, 0);
}

static void flip_in_null(void *context)
{
    (void)context;
    tb_bit_flip(NULL, 0, 0);
}

static void find_set_in_null(void *context)
{
    (void)context;
    (void)tb_bit_find_set(NULL, 0);
}

static void find_clear_in_null(void *context)
{
    (void)context;
    (void)tb_bit_find_clear(NULL, 0);
}

static void next_set_in_null(void *context)
{
    tb_bit_cursor cursor = {0};
    size_t index = {0};

    (void)context;
    (void)tb_bit_next_set(NULL, &cursor, &index);
}

static void visit_null(void *context)
{
    (void)context;
    tb_bit_visit(NULL, ignore_visit, NULL);
}

static void copy_null(void *context)
{
    (void)context;
    (void)tb_bit_copy(NULL, NULL);
}

TB_TEST(core_bit, a_null_vector_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        length_of_null,     count_of_null, get_from_null, put_into_null,
        set_in_null,        clear_in_null, flip_in_null,  find_set_in_null,
        find_clear_in_null, visit_null,    copy_null,     next_set_in_null,
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], NULL, "bits != NULL");
    }
}

typedef struct pair_request
{
    tb_bit *left;
    tb_bit *right;
} pair_request;

static void equal_pair(void *context)
{
    const pair_request *const request = context;

    (void)tb_bit_equal(request->left, request->right);
}

static void subset_pair(void *context)
{
    const pair_request *const request = context;

    (void)tb_bit_is_subset(request->left, request->right);
}

static void proper_subset_pair(void *context)
{
    const pair_request *const request = context;

    (void)tb_bit_is_proper_subset(request->left, request->right);
}

static void union_pair(void *context)
{
    const pair_request *const request = context;

    tb_bit_union(request->left, request->right);
}

static void intersection_pair(void *context)
{
    const pair_request *const request = context;

    tb_bit_intersection(request->left, request->right);
}

static void difference_pair(void *context)
{
    const pair_request *const request = context;

    tb_bit_difference(request->left, request->right);
}

static void symmetric_difference_pair(void *context)
{
    const pair_request *const request = context;

    tb_bit_symmetric_difference(request->left, request->right);
}

TB_TEST(core_bit, vectors_that_do_not_pair_violate_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        equal_pair,        subset_pair,     proper_subset_pair,        union_pair,
        intersection_pair, difference_pair, symmetric_difference_pair,
    };
    pair_request different_lengths = {0};
    pair_request missing_left = {0};
    pair_request missing_right = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    different_lengths.left = tb_bit_create(NULL, 64);
    different_lengths.right = tb_bit_create(NULL, 65);
    missing_left.right = different_lengths.right;
    missing_right.left = different_lengths.left;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], &different_lengths,
                            "the vectors must have the same length");
        TB_TEST_EXPECT_TRAP(bodies[index], &missing_left, "left != NULL");
        TB_TEST_EXPECT_TRAP(bodies[index], &missing_right, "right != NULL");
    }
    tb_bit_destroy(different_lengths.right);
    tb_bit_destroy(different_lengths.left);
}

typedef struct range_request
{
    tb_bit *bits;
    size_t begin;
    size_t end;
} range_request;

static void get_at(void *context)
{
    const range_request *const request = context;

    (void)tb_bit_get(request->bits, request->begin);
}

static void put_at(void *context)
{
    const range_request *const request = context;

    (void)tb_bit_put(request->bits, request->begin, true);
}

static void set_range(void *context)
{
    const range_request *const request = context;

    tb_bit_set(request->bits, request->begin, request->end);
}

static void clear_range(void *context)
{
    const range_request *const request = context;

    tb_bit_clear(request->bits, request->begin, request->end);
}

static void flip_range(void *context)
{
    const range_request *const request = context;

    tb_bit_flip(request->bits, request->begin, request->end);
}

static void find_set_from(void *context)
{
    const range_request *const request = context;

    (void)tb_bit_find_set(request->bits, request->begin);
}

static void find_clear_from(void *context)
{
    const range_request *const request = context;

    (void)tb_bit_find_clear(request->bits, request->begin);
}

static void visit_without_a_visitor(void *context)
{
    tb_bit_visit(context, NULL, NULL);
}

static void next_set_without_a_cursor(void *context)
{
    size_t index = {0};

    (void)tb_bit_next_set(context, NULL, &index);
}

static void next_set_without_an_index(void *context)
{
    tb_bit_cursor cursor = {0};

    (void)tb_bit_next_set(context, &cursor, NULL);
}

TB_TEST(core_bit, positions_outside_the_vector_violate_the_contract)
{
    static const char *const range_text = "the range must be in order and within the vector";
    range_request request = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    request.bits = tb_bit_create(NULL, 100);

    request.begin = 100;
    TB_TEST_EXPECT_TRAP(get_at, &request, "index must be less than the length");
    TB_TEST_EXPECT_TRAP(put_at, &request, "index must be less than the length");
    request.begin = 101;
    TB_TEST_EXPECT_TRAP(find_set_from, &request, "the search must start within the vector");
    TB_TEST_EXPECT_TRAP(find_clear_from, &request, "the search must start within the vector");

    /* A range that ends beyond the vector, and one that runs backward. */
    request.begin = 50;
    request.end = 101;
    TB_TEST_EXPECT_TRAP(set_range, &request, range_text);
    TB_TEST_EXPECT_TRAP(clear_range, &request, range_text);
    TB_TEST_EXPECT_TRAP(flip_range, &request, range_text);
    request.begin = 60;
    request.end = 59;
    TB_TEST_EXPECT_TRAP(set_range, &request, range_text);
    TB_TEST_EXPECT_TRAP(clear_range, &request, range_text);
    TB_TEST_EXPECT_TRAP(flip_range, &request, range_text);

    TB_TEST_EXPECT_TRAP(visit_without_a_visitor, request.bits, "visitor != NULL");
    TB_TEST_EXPECT_TRAP(next_set_without_a_cursor, request.bits, "cursor != NULL");
    TB_TEST_EXPECT_TRAP(next_set_without_an_index, request.bits, "index != NULL");
    TB_TEST_EXPECT_UINT(tb_bit_count(request.bits), ==, 0);
    tb_bit_destroy(request.bits);
}
