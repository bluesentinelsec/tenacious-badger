/* The atomic values of badger/platform.h must do each operation as one
 * indivisible step. Each is checked alone, for what it returns and leaves
 * behind, and then from several threads at once, where a step that can be
 * divided loses or duplicates something that the case can count.
 *
 * A thread started by a case only records; the case checks after joining. */
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>

/* More threads than a hosted CI machine has cores. */
#define THREAD_COUNT 8

/* Operations each thread performs. Enough that threads without atomic steps
 * collide many times over. */
#define ROUND_COUNT ((size_t)50000)

/* Gives every argument a thread, all running at once, and joins them. */
static void run_on_threads(tb_thread_function *entry, void *arguments, size_t argument_size)
{
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], entry, (char *)arguments + index * argument_size);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

/* ---------------------------------------------------------------------------
 * A size.
 * ------------------------------------------------------------------------- */

TB_TEST(platform_atomic_size, adds_and_subtracts_and_wraps_around)
{
    tb_platform_atomic_size atomic = {0};

    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&atomic), ==, 0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_add(&atomic, 5), ==, 5);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_add(&atomic, 0), ==, 5);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_subtract(&atomic, 2), ==, 3);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&atomic), ==, 3);
    /* Like any size_t, the value wraps around at both ends. */
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_subtract(&atomic, 4), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_add(&atomic, 2), ==, 1);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_add(&atomic, SIZE_MAX), ==, 0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&atomic), ==, 0);
}

static void add_three_and_subtract_one(void *argument)
{
    tb_platform_atomic_size *const atomic = *(tb_platform_atomic_size **)argument;

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        (void)tb_platform_atomic_size_add(atomic, 3);
        (void)tb_platform_atomic_size_subtract(atomic, 1);
    }
}

TB_TEST(platform_atomic_size, no_addition_is_lost_between_threads)
{
    tb_platform_atomic_size atomic = {0};
    tb_platform_atomic_size *arguments[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        arguments[index] = &atomic;
    }
    run_on_threads(add_three_and_subtract_one, arguments, sizeof(arguments[0]));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&atomic), ==, THREAD_COUNT * ROUND_COUNT * 2);
}

/* Values at the ends of the range and on both sides of the top bit, of the
 * whole size and of its lower half. */
static const size_t size_values[] = {
    0, 1, (size_t)UINT32_MAX, SIZE_MAX / 2, SIZE_MAX / 2 + 1, SIZE_MAX - 1, SIZE_MAX,
};

TB_TEST(platform_atomic_size, stores_loads_and_exchanges_every_value)
{
    tb_platform_atomic_size atomic = {0};
    size_t previous = 0;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(size_values); index++)
    {
        TB_TEST_CONTEXT("value %zu", index);
        tb_platform_atomic_size_store(&atomic, size_values[index]);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_size_load(&atomic), ==, size_values[index]);
        TB_TEST_EXPECT_HEX(atomic.value, ==, size_values[index]);
    }
    tb_platform_atomic_size_store(&atomic, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(size_values); index++)
    {
        TB_TEST_CONTEXT("value %zu", index);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_size_exchange(&atomic, size_values[index]), ==,
                           previous);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_size_load(&atomic), ==, size_values[index]);
        previous = size_values[index];
    }
}

TB_TEST(platform_atomic_size, compare_exchange_writes_only_over_the_expected_value)
{
    for (size_t held = 0; held < TB_TEST_COUNT_OF(size_values); held++)
    {
        for (size_t guess = 0; guess < TB_TEST_COUNT_OF(size_values); guess++)
        {
            /* A value that is neither the one held nor the one expected. */
            const size_t desired = 0x12345678;
            tb_platform_atomic_size atomic = {.value = size_values[held]};
            size_t expected = size_values[guess];
            const bool is_as_expected = held == guess;
            const bool was_written =
                tb_platform_atomic_size_compare_exchange(&atomic, &expected, desired);

            TB_TEST_CONTEXT("holding value %zu, expecting value %zu", held, guess);
            TB_TEST_EXPECT(was_written == is_as_expected);
            /* Either way, `expected` ends as the value that was found. */
            TB_TEST_EXPECT_HEX(expected, ==, size_values[held]);
            if (was_written)
            {
                TB_TEST_EXPECT_HEX(tb_platform_atomic_size_load(&atomic), ==, desired);
            }
            else
            {
                TB_TEST_EXPECT_HEX(tb_platform_atomic_size_load(&atomic), ==, size_values[held]);
            }
        }
    }
}

/* Adds three as a change that is computed from a reading: read, compute, and
 * write only if no other thread wrote in between. */
static void add_three_by_compare_exchange(void *argument)
{
    tb_platform_atomic_size *const atomic = *(tb_platform_atomic_size **)argument;

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        size_t seen = tb_platform_atomic_size_load(atomic);

        while (!tb_platform_atomic_size_compare_exchange(atomic, &seen, seen + 3))
        {
            /* `seen` now holds what another thread wrote. */
        }
    }
}

TB_TEST(platform_atomic_size, compare_exchange_loses_no_change_between_threads)
{
    tb_platform_atomic_size atomic = {0};
    tb_platform_atomic_size *arguments[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        arguments[index] = &atomic;
    }
    run_on_threads(add_three_by_compare_exchange, arguments, sizeof(arguments[0]));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&atomic), ==, THREAD_COUNT * ROUND_COUNT * 3);
}

/* ---------------------------------------------------------------------------
 * A 32-bit value.
 * ------------------------------------------------------------------------- */

/* Values at the ends of the range and on both sides of the top bit, which an
 * adapter built on signed operations must carry through unchanged. */
static const uint32_t u32_values[] = {
    0, 1, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xDEADBEEF, UINT32_MAX - 1, UINT32_MAX,
};

TB_TEST(platform_atomic_u32, stores_and_loads_every_value)
{
    tb_platform_atomic_u32 atomic = {0};

    TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&atomic), ==, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(u32_values); index++)
    {
        TB_TEST_CONTEXT("value %zu", index);
        tb_platform_atomic_u32_store(&atomic, u32_values[index]);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_u32_load(&atomic), ==, u32_values[index]);
        TB_TEST_EXPECT_HEX(atomic.value, ==, u32_values[index]);
    }
}

TB_TEST(platform_atomic_u32, exchange_returns_the_value_it_replaced)
{
    tb_platform_atomic_u32 atomic = {0};
    uint32_t previous = 0;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(u32_values); index++)
    {
        TB_TEST_CONTEXT("value %zu", index);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_u32_exchange(&atomic, u32_values[index]), ==,
                           previous);
        TB_TEST_EXPECT_HEX(tb_platform_atomic_u32_load(&atomic), ==, u32_values[index]);
        previous = u32_values[index];
    }
}

TB_TEST(platform_atomic_u32, compare_exchange_writes_only_over_the_expected_value)
{
    for (size_t held = 0; held < TB_TEST_COUNT_OF(u32_values); held++)
    {
        for (size_t guess = 0; guess < TB_TEST_COUNT_OF(u32_values); guess++)
        {
            /* A value that is neither the one held nor the one expected. */
            const uint32_t desired = 0x12345678;
            tb_platform_atomic_u32 atomic = {.value = u32_values[held]};
            uint32_t expected = u32_values[guess];
            const bool is_as_expected = held == guess;
            const bool was_written =
                tb_platform_atomic_u32_compare_exchange(&atomic, &expected, desired);

            TB_TEST_CONTEXT("holding value %zu, expecting value %zu", held, guess);
            TB_TEST_EXPECT(was_written == is_as_expected);
            /* Either way, `expected` ends as the value that was found. */
            TB_TEST_EXPECT_HEX(expected, ==, u32_values[held]);
            if (was_written)
            {
                TB_TEST_EXPECT_HEX(tb_platform_atomic_u32_load(&atomic), ==, desired);
            }
            else
            {
                TB_TEST_EXPECT_HEX(tb_platform_atomic_u32_load(&atomic), ==, u32_values[held]);
            }
        }
    }
}

static void add_one_by_compare_exchange(void *argument)
{
    tb_platform_atomic_u32 *const atomic = *(tb_platform_atomic_u32 **)argument;

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        uint32_t seen = tb_platform_atomic_u32_load(atomic);

        while (!tb_platform_atomic_u32_compare_exchange(atomic, &seen, seen + 1))
        {
            /* Another thread wrote first; `seen` holds what it wrote. */
        }
    }
}

/* An addition made of a reading and a conditional write loses nothing only if
 * the write refuses every value but the one that was read. */
TB_TEST(platform_atomic_u32, compare_exchange_loses_no_addition_between_threads)
{
    tb_platform_atomic_u32 atomic = {0};
    tb_platform_atomic_u32 *arguments[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        arguments[index] = &atomic;
    }
    run_on_threads(add_one_by_compare_exchange, arguments, sizeof(arguments[0]));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_u32_load(&atomic), ==, THREAD_COUNT * ROUND_COUNT);
}

/* One more token than there are threads: every thread holds one, and one is
 * in the atomic value. A thread swaps the token it holds for the one in the
 * value, again and again. */
#define TOKEN_COUNT (THREAD_COUNT + 1)

typedef struct u32_trader
{
    tb_platform_atomic_u32 *shared;
    uint32_t token; /* The token the thread holds. */
} u32_trader;

static void trade_u32_tokens(void *argument)
{
    u32_trader *const trader = argument;

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        trader->token = tb_platform_atomic_u32_exchange(trader->shared, trader->token);
    }
}

/* If an exchange could be divided, two threads would at some point take the
 * same token out, and another token would be gone for good. */
TB_TEST(platform_atomic_u32, exchange_neither_loses_nor_duplicates_between_threads)
{
    tb_platform_atomic_u32 shared = {.value = THREAD_COUNT};
    u32_trader traders[THREAD_COUNT] = {0};
    size_t seen_counts[TOKEN_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        traders[index].shared = &shared;
        traders[index].token = (uint32_t)index;
    }
    run_on_threads(trade_u32_tokens, traders, sizeof(traders[0]));

    TB_TEST_REQUIRE_UINT(tb_platform_atomic_u32_load(&shared), <, TOKEN_COUNT);
    seen_counts[tb_platform_atomic_u32_load(&shared)]++;
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_REQUIRE_UINT(traders[index].token, <, TOKEN_COUNT);
        seen_counts[traders[index].token]++;
    }
    for (size_t token = 0; token < TOKEN_COUNT; token++)
    {
        TB_TEST_CONTEXT("token %zu", token);
        TB_TEST_EXPECT_UINT(seen_counts[token], ==, 1);
    }
}

/* ---------------------------------------------------------------------------
 * A pointer.
 * ------------------------------------------------------------------------- */

TB_TEST(platform_atomic_pointer, stores_loads_and_exchanges_pointers)
{
    tb_platform_atomic_pointer atomic = {0};
    int first = 1;
    int second = 2;

    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, NULL);
    tb_platform_atomic_pointer_store(&atomic, &first);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, &first);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_exchange(&atomic, &second), ==, &first);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, &second);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_exchange(&atomic, NULL), ==, &second);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_exchange(&atomic, &first), ==, NULL);
    tb_platform_atomic_pointer_store(&atomic, NULL);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, NULL);
}

TB_TEST(platform_atomic_pointer, compare_exchange_writes_only_over_the_expected_pointer)
{
    int first = 1;
    int second = 2;
    int third = 3;
    /* NULL is a pointer like any other, to hold, to expect, and to write. */
    void *const pointers[] = {NULL, &first, &second};

    for (size_t held = 0; held < TB_TEST_COUNT_OF(pointers); held++)
    {
        for (size_t guess = 0; guess < TB_TEST_COUNT_OF(pointers); guess++)
        {
            tb_platform_atomic_pointer atomic = {.value = pointers[held]};
            void *expected = pointers[guess];
            const bool is_as_expected = held == guess;
            const bool was_written =
                tb_platform_atomic_pointer_compare_exchange(&atomic, &expected, &third);

            TB_TEST_CONTEXT("holding pointer %zu, expecting pointer %zu", held, guess);
            TB_TEST_EXPECT(was_written == is_as_expected);
            TB_TEST_EXPECT_POINTER(expected, ==, pointers[held]);
            if (was_written)
            {
                TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, &third);
            }
            else
            {
                TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==,
                                       pointers[held]);
            }
        }
    }
}

TB_TEST(platform_atomic_pointer, compare_exchange_writes_null_over_the_expected_pointer)
{
    int object = 1;
    tb_platform_atomic_pointer atomic = {.value = &object};
    void *expected = &object;

    TB_TEST_EXPECT(tb_platform_atomic_pointer_compare_exchange(&atomic, &expected, NULL));
    TB_TEST_EXPECT_POINTER(expected, ==, &object);
    TB_TEST_EXPECT_POINTER(tb_platform_atomic_pointer_load(&atomic), ==, NULL);
}

/* Nodes each thread pushes. Fewer than ROUND_COUNT, because every one is an
 * object of the case. */
#define NODES_PER_THREAD ((size_t)2000)

typedef struct node
{
    struct node *next;
    size_t visit_count; /* Times the case met the node when it walked the list. */
} node;

typedef struct pusher
{
    tb_platform_atomic_pointer *top; /* The newest node of a list that all threads share. */
    node nodes[NODES_PER_THREAD];
} pusher;

static void push_nodes(void *argument)
{
    pusher *const self = argument;

    for (size_t index = 0; index < NODES_PER_THREAD; index++)
    {
        node *const pushed = &self->nodes[index];
        void *seen = tb_platform_atomic_pointer_load(self->top);

        pushed->next = seen;
        while (!tb_platform_atomic_pointer_compare_exchange(self->top, &seen, pushed))
        {
            /* Another thread pushed first; go on top of its node instead. */
            pushed->next = seen;
        }
    }
}

/* Threads push onto one list by pointing a new node at the top they read and
 * then replacing that top. A replacement that did not refuse a changed top
 * would cut the nodes pushed in between out of the list. */
TB_TEST(platform_atomic_pointer, compare_exchange_keeps_every_node_pushed_between_threads)
{
    static pusher pushers[THREAD_COUNT] = {0};
    tb_platform_atomic_pointer top = {0};
    size_t node_count = 0;
    size_t wrong_count = 0;

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        pushers[index].top = &top;
        for (size_t nth = 0; nth < NODES_PER_THREAD; nth++)
        {
            pushers[index].nodes[nth] = (node){0};
        }
    }
    run_on_threads(push_nodes, pushers, sizeof(pushers[0]));

    /* The limit keeps a list that was tied into a loop from being walked forever. */
    for (node *current = tb_platform_atomic_pointer_load(&top);
         current != NULL && node_count <= THREAD_COUNT * NODES_PER_THREAD; current = current->next)
    {
        current->visit_count++;
        node_count++;
    }
    TB_TEST_EXPECT_UINT(node_count, ==, THREAD_COUNT * NODES_PER_THREAD);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        for (size_t nth = 0; nth < NODES_PER_THREAD; nth++)
        {
            if (pushers[index].nodes[nth].visit_count != 1)
            {
                wrong_count++;
            }
        }
    }
    TB_TEST_EXPECT_UINT(wrong_count, ==, 0);
}

typedef struct pointer_trader
{
    tb_platform_atomic_pointer *shared;
    size_t *token; /* The token the thread holds: one of the case's counters. */
} pointer_trader;

static void trade_pointer_tokens(void *argument)
{
    pointer_trader *const trader = argument;

    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        trader->token = tb_platform_atomic_pointer_exchange(trader->shared, trader->token);
    }
}

TB_TEST(platform_atomic_pointer, exchange_neither_loses_nor_duplicates_between_threads)
{
    size_t seen_counts[TOKEN_COUNT] = {0};
    tb_platform_atomic_pointer shared = {.value = &seen_counts[THREAD_COUNT]};
    pointer_trader traders[THREAD_COUNT] = {0};
    size_t *last_token = NULL;

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        traders[index].shared = &shared;
        traders[index].token = &seen_counts[index];
    }
    run_on_threads(trade_pointer_tokens, traders, sizeof(traders[0]));

    /* Each token is a counter, so holding one twice shows as a count of two. */
    last_token = tb_platform_atomic_pointer_load(&shared);
    TB_TEST_REQUIRE(last_token >= &seen_counts[0] && last_token <= &seen_counts[THREAD_COUNT]);
    (*last_token)++;
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_REQUIRE(traders[index].token >= &seen_counts[0] &&
                        traders[index].token <= &seen_counts[THREAD_COUNT]);
        (*traders[index].token)++;
    }
    for (size_t token = 0; token < TOKEN_COUNT; token++)
    {
        TB_TEST_CONTEXT("token %zu", token);
        TB_TEST_EXPECT_UINT(seen_counts[token], ==, 1);
    }
}
