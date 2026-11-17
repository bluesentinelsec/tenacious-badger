/* A tb_thread_local is a place where every thread keeps a pointer of its
 * own. The cases check what that means: a thread sees what it stored and
 * never what another stored, a thread that has stored nothing sees NULL, and
 * the value of a thread that ends is given to the destructor exactly once.
 *
 * A thread started by a case only records; the case checks after joining. */
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

#define THREAD_COUNT 8
#define ROUNDS ((size_t)2000)

typedef struct keeper
{
    tb_thread_local *local;
    tb_thread_local *other;                /* A second place, or NULL. */
    bool stores_nothing;                   /* Leaves its value NULL. */
    size_t wrong_count;                    /* Reads that gave something else than was stored. */
    tb_platform_atomic_size destroy_count; /* Times the destructor was given this keeper. */
} keeper;

static void count_unless(keeper *self, bool is_right)
{
    if (!is_right)
    {
        self->wrong_count++;
    }
}

/* Stores itself and reads itself back, over and over, while the other
 * threads do the same with themselves. */
static void keep_a_value_of_its_own(void *argument)
{
    keeper *const self = argument;

    count_unless(self, tb_thread_local_get(self->local) == NULL);
    if (self->stores_nothing)
    {
        return;
    }
    for (size_t round = 0; round < ROUNDS; round++)
    {
        tb_thread_local_set(self->local, self);
        if (self->other != NULL)
        {
            /* What goes in one place is not what comes out of another. */
            count_unless(self, tb_thread_local_get(self->other) == NULL || round > 0);
            tb_thread_local_set(self->other, &self->wrong_count);
            count_unless(self, tb_thread_local_get(self->other) == &self->wrong_count);
        }
        tb_thread_relax();
        count_unless(self, tb_thread_local_get(self->local) == self);
    }
    if (self->other != NULL)
    {
        tb_thread_local_set(self->other, NULL);
    }
}

static void run_keepers(keeper *keepers)
{
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], keep_a_value_of_its_own, &keepers[index]);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

TB_TEST(platform_thread_local, every_thread_has_a_value_of_its_own)
{
    tb_thread_local local = {0};
    tb_thread_local other = {0};
    keeper keepers[THREAD_COUNT] = {0};
    int mine = 0;

    tb_thread_local_create(&local, NULL);
    tb_thread_local_create(&other, NULL);
    TB_TEST_EXPECT_POINTER(tb_thread_local_get(&local), ==, NULL);
    tb_thread_local_set(&local, &mine);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        keepers[index].local = &local;
        keepers[index].other = &other;
    }
    run_keepers(keepers);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_CONTEXT("thread %zu", index);
        TB_TEST_EXPECT_UINT(keepers[index].wrong_count, ==, 0);
    }
    /* What the other threads did left this thread's value alone. */
    TB_TEST_EXPECT_POINTER(tb_thread_local_get(&local), ==, &mine);
    TB_TEST_EXPECT_POINTER(tb_thread_local_get(&other), ==, NULL);
    tb_thread_local_set(&local, NULL);
    tb_thread_local_destroy(&local);
    tb_thread_local_destroy(&other);
    TB_TEST_EXPECT_UINT(local.key, ==, 0);
}

static void count_the_keeper(void *value)
{
    keeper *const self = value;

    (void)tb_platform_atomic_size_add(&self->destroy_count, 1);
}

/* Each thread stores itself, and one stores nothing. When the threads have
 * ended, the destructor has been given every thread that stored something,
 * once, and has not been called for the one that did not. */
TB_TEST(platform_thread_local, the_value_of_a_thread_that_ends_goes_to_the_destructor_once)
{
    tb_thread_local local = {0};
    keeper keepers[THREAD_COUNT] = {0};

    tb_thread_local_create(&local, count_the_keeper);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        keepers[index].local = &local;
    }
    keepers[3].stores_nothing = true;
    run_keepers(keepers);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        size_t expected = 1;

        TB_TEST_CONTEXT("thread %zu", index);
        if (index == 3)
        {
            expected = 0;
        }
        TB_TEST_EXPECT_UINT(keepers[index].wrong_count, ==, 0);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&keepers[index].destroy_count), ==,
                            expected);
    }
    /* The thread of the case stored nothing, so nothing is left to destroy. */
    tb_thread_local_destroy(&local);
}

/* A place that is destroyed and made again starts with NULL in every thread,
 * this one included, whatever it held before. */
TB_TEST(platform_thread_local, a_place_made_again_starts_empty)
{
    tb_thread_local local = {0};
    int mine = 0;

    for (size_t round = 0; round < 50; round++)
    {
        tb_thread_local_create(&local, NULL);
        TB_TEST_EXPECT_POINTER(tb_thread_local_get(&local), ==, NULL);
        tb_thread_local_set(&local, &mine);
        TB_TEST_EXPECT_POINTER(tb_thread_local_get(&local), ==, &mine);
        tb_thread_local_destroy(&local);
    }
}

/* --- contracts ------------------------------------------------------------------- */

static void create_null(void *context)
{
    (void)context;
    tb_thread_local_create(NULL, NULL);
}

static void create_again(void *context)
{
    tb_thread_local_create(context, NULL);
}

static void destroy_it(void *context)
{
    tb_thread_local_destroy(context);
}

static void get_from_it(void *context)
{
    (void)tb_thread_local_get(context);
}

static void set_in_it(void *context)
{
    tb_thread_local_set(context, NULL);
}

TB_TEST(platform_thread_local, bad_arguments_violate_the_contract)
{
    tb_thread_local never_created = {0};
    tb_thread_local local = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_null, NULL, "local != NULL");
    TB_TEST_EXPECT_TRAP(destroy_it, NULL, "local != NULL");
    TB_TEST_EXPECT_TRAP(get_from_it, NULL, "local != NULL");
    TB_TEST_EXPECT_TRAP(set_in_it, NULL, "local != NULL");

    TB_TEST_EXPECT_TRAP(destroy_it, &never_created, "is not created");
    TB_TEST_EXPECT_TRAP(get_from_it, &never_created, "is not created");
    TB_TEST_EXPECT_TRAP(set_in_it, &never_created, "is not created");

    tb_thread_local_create(&local, NULL);
    TB_TEST_EXPECT_TRAP(create_again, &local, "is already created");
    tb_thread_local_destroy(&local);
    TB_TEST_EXPECT_TRAP(destroy_it, &local, "is not created");
}
