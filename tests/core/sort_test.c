/* Sorting must put every array in order, keep every element, and give the
 * same order on every platform, equal elements included. tb_sort_stable must
 * also keep equal elements in the order they had, and tb_sort must stay
 * O(n log n) even on input built to defeat its choice of pivot.
 *
 * Arrays are built in many shapes (random, sorted, reversed, all equal, few
 * distinct keys, organ pipe, sawtooth) at every short length and a few long
 * ones, and with elements of several sizes, since moving an element is done
 * differently for 4, 8, and 16 bytes than for any other size. */
#include "badger/assert.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "badger/sort.h"

#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Elements of any size.
 *
 * An element is `size` bytes. Its key, which is all the comparison looks
 * at, is its first byte when it has fewer than 4, else its first 4 bytes as
 * a uint32_t. The rest of an element is a pattern that differs from element
 * to element, so that an element that was lost, duplicated, or torn apart by
 * a faulty move is noticed.
 * ------------------------------------------------------------------------- */

/* Sizes that take each way of moving an element: 4, 8, and 16 bytes have
 * their own; 24 and 64 bytes, multiples of 8 up to the largest, move a word
 * at a time; 1, 3, and 72 bytes take the general way, and 100 bytes is moved
 * in pieces. */
static const size_t element_sizes[] = {1, 3, 4, 8, 16, 24, 64, 72, 100};

/* Lengths beyond the short ones, past the insertion sort and the ninther. */
static const size_t long_counts[] = {100, 129, 1000, 4099};

#define SHORT_COUNT_LIMIT ((size_t)40)

typedef enum shape
{
    SHAPE_RANDOM,
    SHAPE_SORTED,
    SHAPE_REVERSED,
    SHAPE_ALL_EQUAL,
    SHAPE_FEW_KEYS,
    SHAPE_ORGAN_PIPE,
    SHAPE_SAWTOOTH,
    SHAPE_COUNT
} shape;

static const char *const shape_names[SHAPE_COUNT] = {
    "random", "sorted", "reversed", "all equal", "few keys", "organ pipe", "sawtooth"};

static uint32_t key_of(const unsigned char *element, size_t size)
{
    uint32_t key = 0;

    if (size < sizeof(key))
    {
        return element[0];
    }
    memcpy(&key, element, sizeof(key));
    return key;
}

static void set_key(unsigned char *element, size_t size, uint32_t key)
{
    if (size < sizeof(key))
    {
        element[0] = (unsigned char)key;
        return;
    }
    memcpy(element, &key, sizeof(key));
}

static uint32_t key_for(shape kind, size_t index, size_t count)
{
    switch (kind)
    {
    case SHAPE_RANDOM:
        return (uint32_t)tb_test_random_u64();
    case SHAPE_SORTED:
        return (uint32_t)index;
    case SHAPE_REVERSED:
        return (uint32_t)(count - index);
    case SHAPE_ALL_EQUAL:
        return 7;
    case SHAPE_FEW_KEYS:
        return (uint32_t)tb_test_random_below(4);
    case SHAPE_ORGAN_PIPE:
        return (uint32_t)(index < count / 2 ? index : count - index);
    case SHAPE_SAWTOOTH:
        return (uint32_t)(index % 9);
    case SHAPE_COUNT:
        break;
    }
    TB_TEST_FAIL("no shape %d", (int)kind);
}

/* Fills `count` elements of `size` bytes with keys of the shape and a pattern
 * of their own after the key. */
static void fill(unsigned char *elements, size_t count, size_t size, shape kind)
{
    for (size_t index = 0; index < count; index++)
    {
        unsigned char *const element = elements + index * size;

        uint32_t key = key_for(kind, index, count);

        for (size_t byte = 0; byte < size; byte++)
        {
            element[byte] = (unsigned char)(index * 31 + byte * 7 + 1);
        }
        /* A key in one byte has fewer than 256 values; this keeps the shapes
         * that count up from wrapping early. */
        if (size < sizeof(uint32_t))
        {
            key %= 251;
        }
        set_key(element, size, key);
    }
}

/* What an array holds, whatever the order: an element moved whole adds the
 * same hash wherever it ends up. */
static uint64_t contents_of(const unsigned char *elements, size_t count, size_t size)
{
    uint64_t sum = 0;

    for (size_t index = 0; index < count; index++)
    {
        sum += tb_hash_bytes(elements + index * size, size);
    }
    return sum;
}

typedef struct comparisons
{
    size_t size;
    size_t count;
    const void *expected_context;
    bool context_was_wrong;
} comparisons;

static int by_key(const void *left, const void *right, void *context)
{
    comparisons *const counted = context;
    const uint32_t first = key_of(left, counted->size);
    const uint32_t second = key_of(right, counted->size);

    counted->count++;
    if (counted->expected_context != NULL && counted->expected_context != context)
    {
        counted->context_was_wrong = true;
    }
    return (first > second) - (first < second);
}

/* The index of the first element out of order, or `count` if there is none. */
static size_t first_out_of_order(const unsigned char *elements, size_t count, size_t size)
{
    for (size_t index = 1; index < count; index++)
    {
        if (key_of(elements + (index - 1) * size, size) > key_of(elements + index * size, size))
        {
            return index;
        }
    }
    return count;
}

typedef enum algorithm
{
    ALGORITHM_SORT,
    ALGORITHM_STABLE
} algorithm;

static void sort_with(algorithm chosen, unsigned char *elements, size_t count, size_t size,
                      comparisons *counted)
{
    if (chosen == ALGORITHM_SORT)
    {
        tb_sort(elements, count, size, by_key, counted);
        return;
    }
    tb_sort_stable(elements, count, size, by_key, counted, NULL);
}

/* Sorts one array of each shape at `count` and `size`, and checks that it is
 * in order and still holds every element. */
static void check_every_shape(algorithm chosen, size_t count, size_t size)
{
    unsigned char *const elements = tb_malloc(count * size + 1);

    for (int kind = 0; kind < SHAPE_COUNT; kind++)
    {
        comparisons counted = {.size = size};
        uint64_t before = 0;

        fill(elements, count, size, (shape)kind);
        before = contents_of(elements, count, size);
        sort_with(chosen, elements, count, size, &counted);
        TB_TEST_CONTEXT("%s, %zu elements of %zu bytes", shape_names[kind], count, size);
        TB_TEST_EXPECT_UINT(first_out_of_order(elements, count, size), ==, count);
        TB_TEST_EXPECT_HEX(contents_of(elements, count, size), ==, before);
    }
    tb_free(elements);
}

static void check_every_length(algorithm chosen)
{
    for (size_t size_index = 0; size_index < TB_TEST_COUNT_OF(element_sizes); size_index++)
    {
        const size_t size = element_sizes[size_index];

        for (size_t count = 0; count <= SHORT_COUNT_LIMIT; count++)
        {
            check_every_shape(chosen, count, size);
        }
        for (size_t index = 0; index < TB_TEST_COUNT_OF(long_counts); index++)
        {
            check_every_shape(chosen, long_counts[index], size);
        }
    }
}

TB_TEST(core_sort, arrays_of_every_shape_length_and_element_size_are_sorted)
{
    check_every_length(ALGORITHM_SORT);
}

TB_TEST(core_sort, stable_sorting_sorts_arrays_of_every_shape_length_and_element_size)
{
    check_every_length(ALGORITHM_STABLE);
}

/* ---------------------------------------------------------------------------
 * Records: a key and the place the record started at.
 * ------------------------------------------------------------------------- */

typedef struct record
{
    uint32_t key;
    uint32_t origin;
} record;

static int by_record_key(const void *left, const void *right, void *context)
{
    const record *const first = left;
    const record *const second = right;

    (void)context;
    return (first->key > second->key) - (first->key < second->key);
}

/* Records whose keys repeat, each knowing where it started. */
static void fill_records(record *records, size_t count, uint32_t distinct_keys)
{
    for (size_t index = 0; index < count; index++)
    {
        records[index].key = (uint32_t)tb_test_random_below(distinct_keys);
        records[index].origin = (uint32_t)index;
    }
}

TB_TEST(core_sort, stable_sorting_keeps_equal_elements_in_the_order_they_had)
{
    static const size_t counts[] = {2, 15, 16, 17, 100, 1000, 5000};
    static const uint32_t distinct_keys[] = {1, 3, 50};

    for (size_t count_index = 0; count_index < TB_TEST_COUNT_OF(counts); count_index++)
    {
        for (size_t keys_index = 0; keys_index < TB_TEST_COUNT_OF(distinct_keys); keys_index++)
        {
            const size_t count = counts[count_index];
            record *const records = tb_malloc(count * sizeof(record));
            size_t disorder = 0;

            fill_records(records, count, distinct_keys[keys_index]);
            tb_sort_stable(records, count, sizeof(record), by_record_key, NULL, NULL);
            for (size_t index = 1; index < count; index++)
            {
                const bool same_key = records[index - 1].key == records[index].key;

                if (records[index - 1].key > records[index].key ||
                    (same_key && records[index - 1].origin > records[index].origin))
                {
                    disorder++;
                }
            }
            TB_TEST_CONTEXT("%zu records, %u keys", count, (unsigned)distinct_keys[keys_index]);
            TB_TEST_EXPECT_UINT(disorder, ==, 0);
            tb_free(records);
        }
    }
}

/* Where equal elements end is unspecified but must not depend on the
 * platform, the compiler, or the build. These records are made the same way
 * everywhere, without the run's random seed, and the order the sort leaves
 * them in is summed into one number that every platform must agree on. If an
 * intended change to the algorithm changes the number, update it here and
 * say so in the change: the order of equal elements moves with it. */
static uint64_t sorted_origins_digest(algorithm chosen)
{
    enum
    {
        DIGEST_COUNT = 3000
    };
    record *const records = tb_malloc(DIGEST_COUNT * sizeof(record));
    uint64_t state = 0x9E3779B97F4A7C15u;
    uint64_t digest = 0;

    for (size_t index = 0; index < DIGEST_COUNT; index++)
    {
        state = state * 6364136223846793005u + 1442695040888963407u;
        records[index].key = (uint32_t)(state >> 59);
        records[index].origin = (uint32_t)index;
    }
    if (chosen == ALGORITHM_SORT)
    {
        tb_sort(records, DIGEST_COUNT, sizeof(record), by_record_key, NULL);
    }
    else
    {
        tb_sort_stable(records, DIGEST_COUNT, sizeof(record), by_record_key, NULL, NULL);
    }
    digest = tb_hash_bytes(records, DIGEST_COUNT * sizeof(record));
    tb_free(records);
    return digest;
}

TB_TEST(core_sort, equal_elements_end_in_the_same_order_on_every_platform)
{
    TB_TEST_EXPECT_HEX(sorted_origins_digest(ALGORITHM_SORT), ==, UINT64_C(0xF5B5A1575431D286));
    TB_TEST_EXPECT_HEX(sorted_origins_digest(ALGORITHM_STABLE), ==, UINT64_C(0xD266A83290B675BE));
}

/* ---------------------------------------------------------------------------
 * The comparison and its context.
 * ------------------------------------------------------------------------- */

TB_TEST(core_sort, every_comparison_receives_the_context)
{
    uint32_t values[200] = {0};
    comparisons counted = {.size = sizeof(values[0])};
    uint32_t key = 77;

    counted.expected_context = &counted;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(values); index++)
    {
        values[index] = (uint32_t)tb_test_random_below(1000);
    }
    tb_sort(values, TB_TEST_COUNT_OF(values), sizeof(values[0]), by_key, &counted);
    tb_sort_stable(values, TB_TEST_COUNT_OF(values), sizeof(values[0]), by_key, &counted, NULL);
    (void)tb_sort_lower_bound(values, TB_TEST_COUNT_OF(values), sizeof(values[0]), &key, by_key,
                              &counted);
    TB_TEST_EXPECT_UINT(counted.count, >, 0);
    TB_TEST_EXPECT(!counted.context_was_wrong);
}

static int must_not_be_called(const void *left, const void *right, void *context)
{
    (void)left;
    (void)right;
    (void)context;
    TB_TEST_FAIL("an array of fewer than two elements needs no comparison");
}

TB_TEST(core_sort, an_array_of_fewer_than_two_elements_is_left_alone)
{
    uint32_t one = 5;

    tb_sort(NULL, 0, sizeof(one), must_not_be_called, NULL);
    tb_sort(&one, 1, sizeof(one), must_not_be_called, NULL);
    tb_sort_stable(NULL, 0, sizeof(one), must_not_be_called, NULL, NULL);
    tb_sort_stable(&one, 1, sizeof(one), must_not_be_called, NULL, NULL);
    TB_TEST_EXPECT_UINT(one, ==, 5);
    TB_TEST_EXPECT_UINT(tb_sort_lower_bound(NULL, 0, sizeof(one), &one, must_not_be_called, NULL),
                        ==, 0);
}

/* A comparison that answers at random is not an order, and the result is in
 * no particular order. But no element may be lost or duplicated, and nothing
 * outside the array may be touched, which the sanitizer builds would catch. */
static int at_random(const void *left, const void *right, void *context)
{
    (void)left;
    (void)right;
    (void)context;
    return (int)tb_test_random_below(3) - 1;
}

TB_TEST(core_sort, a_comparison_that_is_not_an_order_loses_no_element)
{
    static const size_t counts[] = {2, 17, 200, 3000};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        const size_t count = counts[index];
        record *const records = tb_malloc(count * sizeof(record));
        uint64_t before = 0;

        fill_records(records, count, 1000);
        before = contents_of((const unsigned char *)records, count, sizeof(record));
        tb_sort(records, count, sizeof(record), at_random, NULL);
        TB_TEST_CONTEXT("%zu records", count);
        TB_TEST_EXPECT_HEX(contents_of((const unsigned char *)records, count, sizeof(record)), ==,
                           before);
        tb_sort_stable(records, count, sizeof(record), at_random, NULL, NULL);
        TB_TEST_EXPECT_HEX(contents_of((const unsigned char *)records, count, sizeof(record)), ==,
                           before);
        tb_free(records);
    }
}

/* ---------------------------------------------------------------------------
 * Cost.
 * ------------------------------------------------------------------------- */

/* McIlroy's adversary ("A Killer Adversary for Quicksort", 1999). The values
 * of the elements are not decided in advance: every element starts as "gas",
 * greater than every decided value, and is given a value only when the sort
 * compares two gas elements, in the way that makes its pivot as poor as
 * possible. The answers are always consistent, so this is a real order, and
 * it drives any quicksort that picks pivots by comparing elements to
 * quadratic time. */
typedef struct adversary
{
    size_t *values;
    size_t gas;
    size_t decided;
    size_t candidate;
    size_t comparisons;
} adversary;

static void decide(adversary *against, size_t element)
{
    against->values[element] = against->decided;
    against->decided++;
}

static int adversarial(const void *left, const void *right, void *context)
{
    adversary *const against = context;
    size_t first = 0;
    size_t second = 0;

    memcpy(&first, left, sizeof(first));
    memcpy(&second, right, sizeof(second));
    against->comparisons++;
    if (against->values[first] == against->gas && against->values[second] == against->gas)
    {
        if (first == against->candidate)
        {
            decide(against, first);
        }
        else
        {
            decide(against, second);
        }
    }
    if (against->values[first] == against->gas)
    {
        against->candidate = first;
    }
    else if (against->values[second] == against->gas)
    {
        against->candidate = second;
    }
    return (against->values[first] > against->values[second]) -
           (against->values[first] < against->values[second]);
}

static size_t log2_of(size_t count)
{
    size_t halvings = 0;

    while (count > 1)
    {
        count /= 2;
        halvings++;
    }
    return halvings;
}

/* How many comparisons `count` elements took against the adversary. */
static size_t comparisons_against_the_adversary(algorithm chosen, size_t count)
{
    size_t *const elements = tb_malloc(count * sizeof(size_t));
    adversary against = {.values = tb_malloc(count * sizeof(size_t)), .gas = count};

    for (size_t index = 0; index < count; index++)
    {
        elements[index] = index;
        against.values[index] = against.gas;
    }
    if (chosen == ALGORITHM_SORT)
    {
        tb_sort(elements, count, sizeof(size_t), adversarial, &against);
    }
    else
    {
        tb_sort_stable(elements, count, sizeof(size_t), adversarial, &against, NULL);
    }
    tb_free(against.values);
    tb_free(elements);
    return against.comparisons;
}

/* A quicksort without the heapsort to fall back on would take about n²/4
 * comparisons here, some 25 million for 10,000 elements. Introsort's bound
 * is a few times n log2 n: partitions up to twice log2 n deep, then heapsort. */
TB_TEST(core_sort, input_built_against_the_pivot_still_costs_n_log_n)
{
    const size_t count = 10000;
    const size_t bound = 6 * count * log2_of(count);

    TB_TEST_EXPECT_UINT(comparisons_against_the_adversary(ALGORITHM_SORT, count), <=, bound);
    TB_TEST_EXPECT_UINT(comparisons_against_the_adversary(ALGORITHM_STABLE, count), <=, bound);
}

TB_TEST(core_sort, stable_sorting_a_sorted_array_costs_one_comparison_per_element)
{
    const size_t count = 5000;
    uint32_t *const values = tb_malloc(count * sizeof(uint32_t));
    comparisons counted = {.size = sizeof(uint32_t)};

    for (size_t index = 0; index < count; index++)
    {
        values[index] = (uint32_t)index;
    }
    tb_sort_stable(values, count, sizeof(uint32_t), by_key, &counted, NULL);
    TB_TEST_EXPECT_UINT(counted.count, <, count);
    tb_free(values);
}

/* ---------------------------------------------------------------------------
 * Memory.
 * ------------------------------------------------------------------------- */

TB_TEST(core_sort, stable_sorting_takes_half_the_array_from_the_allocator_and_gives_it_back)
{
    uint64_t values[1001] = {0};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    comparisons counted = {.size = sizeof(values[0])};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(values); index++)
    {
        values[index] = tb_test_random_u64();
    }
    tb_sort_stable(values, TB_TEST_COUNT_OF(values), sizeof(values[0]), by_key, &counted,
                   &allocator);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.peak_byte_count, ==, 500 * sizeof(values[0]));
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    /* A short array is insertion-sorted, with no memory at all. */
    tb_sort_stable(values, 16, sizeof(values[0]), by_key, &counted, &allocator);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
}

/* Elements copied to the scratch memory are passed to the comparison there,
 * so the memory must be aligned as the array's elements are. */
typedef struct wide
{
    _Alignas(32) unsigned char bytes[64];
} wide;

static int by_first_byte_checking_alignment(const void *left, const void *right, void *context)
{
    bool *const misaligned = context;
    const wide *const first = left;
    const wide *const second = right;

    if ((uintptr_t)left % _Alignof(wide) != 0 || (uintptr_t)right % _Alignof(wide) != 0)
    {
        *misaligned = true;
    }
    return (first->bytes[0] > second->bytes[0]) - (first->bytes[0] < second->bytes[0]);
}

TB_TEST(core_sort, the_scratch_memory_is_aligned_as_the_elements_are)
{
    enum
    {
        WIDE_COUNT = 300
    };
    wide *const elements = tb_aligned_alloc(_Alignof(wide), WIDE_COUNT * sizeof(wide));
    bool misaligned = false;

    for (size_t index = 0; index < WIDE_COUNT; index++)
    {
        tb_test_random_bytes(elements[index].bytes, sizeof(elements[index].bytes));
    }
    tb_sort_stable(elements, WIDE_COUNT, sizeof(wide), by_first_byte_checking_alignment,
                   &misaligned, NULL);
    TB_TEST_EXPECT(!misaligned);
    tb_aligned_free(elements);
}

/* ---------------------------------------------------------------------------
 * Finding a place.
 * ------------------------------------------------------------------------- */

TB_TEST(core_sort, the_lower_bound_is_the_first_element_not_before_the_key)
{
    /* Keys with runs, gaps, and both ends. */
    static const uint32_t sorted[] = {2, 4, 4, 4, 7, 9, 9, 15};
    static const struct
    {
        uint32_t key;
        size_t expected;
    } cases[] = {
        {0, 0}, {2, 0}, {3, 1}, {4, 1}, {5, 4}, {7, 4}, {8, 5}, {9, 5}, {10, 7}, {15, 7}, {16, 8},
    };
    comparisons counted = {.size = sizeof(uint32_t)};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("key %u", (unsigned)cases[index].key);
        TB_TEST_EXPECT_UINT(tb_sort_lower_bound(sorted, TB_TEST_COUNT_OF(sorted), sizeof(sorted[0]),
                                                &cases[index].key, by_key, &counted),
                            ==, cases[index].expected);
    }
}

/* Records found by a name: the key is a string, not a record. */
typedef struct named
{
    const char *name;
    int value;
} named;

static int record_before_name(const void *element, const void *key, void *context)
{
    const named *const entry = element;
    const char *const name = key;
    size_t index = 0;

    (void)context;
    while (entry->name[index] != '\0' && entry->name[index] == name[index])
    {
        index++;
    }
    return (int)(unsigned char)entry->name[index] - (int)(unsigned char)name[index];
}

TB_TEST(core_sort, the_key_may_be_a_different_type_from_the_elements)
{
    static const named records[] = {{"ammo", 1}, {"health", 2}, {"shield", 3}, {"speed", 4}};
    const size_t count = TB_TEST_COUNT_OF(records);
    const size_t found =
        tb_sort_lower_bound(records, count, sizeof(records[0]), "shield", record_before_name, NULL);
    const size_t missing =
        tb_sort_lower_bound(records, count, sizeof(records[0]), "jump", record_before_name, NULL);

    TB_TEST_REQUIRE_UINT(found, <, count);
    TB_TEST_EXPECT_INT(records[found].value, ==, 3);
    /* Absent: where it would be inserted, between "health" and "shield". */
    TB_TEST_EXPECT_UINT(missing, ==, 2);
}

TB_TEST(core_sort, the_lower_bound_takes_log2_n_comparisons)
{
    const size_t count = 100000;
    uint32_t *const values = tb_malloc(count * sizeof(uint32_t));
    comparisons counted = {.size = sizeof(uint32_t)};
    const uint32_t key = 31337;

    for (size_t index = 0; index < count; index++)
    {
        values[index] = (uint32_t)index;
    }
    TB_TEST_EXPECT_UINT(
        tb_sort_lower_bound(values, count, sizeof(uint32_t), &key, by_key, &counted), ==, 31337);
    TB_TEST_EXPECT_UINT(counted.count, <=, log2_of(count) + 1);
    tb_free(values);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static int compare_nothing(const void *left, const void *right, void *context)
{
    (void)left;
    (void)right;
    (void)context;
    return 0;
}

static void sort_null_elements(void *context)
{
    (void)context;
    tb_sort(NULL, 2, 4, compare_nothing, NULL);
}

static void sort_elements_of_no_size(void *context)
{
    uint32_t values[2] = {0};

    (void)context;
    tb_sort(values, 2, 0, compare_nothing, NULL);
}

static void sort_without_a_comparison(void *context)
{
    uint32_t values[2] = {0};

    (void)context;
    tb_sort(values, 2, sizeof(values[0]), NULL, NULL);
}

static void sort_an_overflowing_array(void *context)
{
    uint32_t values[2] = {0};

    (void)context;
    tb_sort(values, SIZE_MAX / 2, 4, compare_nothing, NULL);
}

static void stable_sort_null_elements(void *context)
{
    (void)context;
    tb_sort_stable(NULL, 2, 4, compare_nothing, NULL, NULL);
}

static void search_without_a_key(void *context)
{
    uint32_t values[2] = {0};

    (void)context;
    (void)tb_sort_lower_bound(values, 2, sizeof(values[0]), NULL, compare_nothing, NULL);
}

static void search_without_a_comparison(void *context)
{
    uint32_t values[2] = {0};
    const uint32_t key = 0;

    (void)context;
    (void)tb_sort_lower_bound(values, 2, sizeof(values[0]), &key, NULL, NULL);
}

TB_TEST(core_sort, bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(sort_null_elements, NULL, "elements may be NULL only when count is 0");
    TB_TEST_EXPECT_TRAP(sort_elements_of_no_size, NULL, "size >= 1");
    TB_TEST_EXPECT_TRAP(sort_without_a_comparison, NULL, "compare != NULL");
    TB_TEST_EXPECT_TRAP(sort_an_overflowing_array, NULL, "count * size must not overflow");
    TB_TEST_EXPECT_TRAP(stable_sort_null_elements, NULL,
                        "elements may be NULL only when count is 0");
    TB_TEST_EXPECT_TRAP(search_without_a_key, NULL, "key != NULL");
    TB_TEST_EXPECT_TRAP(search_without_a_comparison, NULL, "compare != NULL");
}
