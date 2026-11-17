/* The thread and atomic functions say what a caller must not do: pass a null
 * pointer, unlock a mutex it does not hold, lock one twice, wait without the
 * mutex, join a thread that is not there. The system's own answer to each is
 * undefined and differs by platform, from a corrupted lock to a thread that
 * never returns. In a build with assertions every one of them must instead
 * fail an assertion, on every platform, before anything is changed.
 *
 * Each violation is made on the thread of the case, because that is where a
 * trap can be caught. Other threads only hold a mutex for it. */
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* Longest that a thread waits for something that must happen. No machine is
 * so slow, so reaching it means that the wake was lost. */
#define PATIENCE_NANOSECONDS (UINT64_C(20) * NANOSECONDS_PER_SECOND)

#define SKIP_WITHOUT_ASSERTIONS()                                                                  \
    do                                                                                             \
    {                                                                                              \
        if (!tb_assert_library_enabled())                                                          \
        {                                                                                          \
            TB_TEST_SKIP("the platform adapter was built without assertions");                     \
        }                                                                                          \
    } while (false)

/* ---------------------------------------------------------------------------
 * Null pointers.
 * ------------------------------------------------------------------------- */

static tb_mutex unused_mutex = {0};
static tb_condition unused_condition = {0};

static void do_nothing(void *argument)
{
    (void)argument;
}

static void size_add_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_size_add(NULL, 1);
}

static void size_subtract_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_size_subtract(NULL, 1);
}

static void size_load_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_size_load(NULL);
}

static void size_store_null(void *context)
{
    (void)context;
    tb_platform_atomic_size_store(NULL, 1);
}

static void size_exchange_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_size_exchange(NULL, 1);
}

static void size_compare_exchange_null(void *context)
{
    size_t expected = 0;

    (void)context;
    (void)tb_platform_atomic_size_compare_exchange(NULL, &expected, 1);
}

static void size_compare_exchange_null_expected(void *context)
{
    tb_platform_atomic_size atomic = {0};

    (void)context;
    (void)tb_platform_atomic_size_compare_exchange(&atomic, NULL, 1);
}

static void u32_load_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_u32_load(NULL);
}

static void u32_store_null(void *context)
{
    (void)context;
    tb_platform_atomic_u32_store(NULL, 1);
}

static void u32_exchange_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_u32_exchange(NULL, 1);
}

static void u32_compare_exchange_null(void *context)
{
    uint32_t expected = 0;

    (void)context;
    (void)tb_platform_atomic_u32_compare_exchange(NULL, &expected, 1);
}

static void u32_compare_exchange_null_expected(void *context)
{
    tb_platform_atomic_u32 atomic = {0};

    (void)context;
    (void)tb_platform_atomic_u32_compare_exchange(&atomic, NULL, 1);
}

static void pointer_load_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_pointer_load(NULL);
}

static void pointer_store_null(void *context)
{
    (void)context;
    tb_platform_atomic_pointer_store(NULL, NULL);
}

static void pointer_exchange_null(void *context)
{
    (void)context;
    (void)tb_platform_atomic_pointer_exchange(NULL, NULL);
}

static void pointer_compare_exchange_null(void *context)
{
    void *expected = NULL;

    (void)context;
    (void)tb_platform_atomic_pointer_compare_exchange(NULL, &expected, NULL);
}

static void pointer_compare_exchange_null_expected(void *context)
{
    tb_platform_atomic_pointer atomic = {0};

    (void)context;
    (void)tb_platform_atomic_pointer_compare_exchange(&atomic, NULL, NULL);
}

static void create_null_thread(void *context)
{
    (void)context;
    tb_thread_create(NULL, do_nothing, NULL);
}

static void create_null_entry(void *context)
{
    tb_thread thread = {0};

    (void)context;
    tb_thread_create(&thread, NULL, NULL);
}

static void join_null(void *context)
{
    (void)context;
    tb_thread_join(NULL);
}

static void lock_null(void *context)
{
    (void)context;
    tb_mutex_lock(NULL);
}

static void try_lock_null(void *context)
{
    (void)context;
    (void)tb_mutex_try_lock(NULL);
}

static void unlock_null(void *context)
{
    (void)context;
    tb_mutex_unlock(NULL);
}

static void wait_null_condition(void *context)
{
    (void)context;
    tb_condition_wait(NULL, &unused_mutex);
}

static void wait_null_mutex(void *context)
{
    (void)context;
    tb_condition_wait(&unused_condition, NULL);
}

static void wait_for_null_condition(void *context)
{
    (void)context;
    (void)tb_condition_wait_for(NULL, &unused_mutex, 1);
}

static void wait_for_null_mutex(void *context)
{
    (void)context;
    (void)tb_condition_wait_for(&unused_condition, NULL, 1);
}

static void wake_one_null(void *context)
{
    (void)context;
    tb_condition_wake_one(NULL);
}

static void wake_all_null(void *context)
{
    (void)context;
    tb_condition_wake_all(NULL);
}

typedef struct null_case
{
    const char *call;        /* The call, for the report of a failure. */
    tb_test_trap_body *body; /* Makes the call. */
    const char *assertion;   /* The condition that must fail. */
} null_case;

static const null_case null_cases[] = {
    {"tb_platform_atomic_size_add(NULL, 1)", size_add_null, "atomic != NULL"},
    {"tb_platform_atomic_size_subtract(NULL, 1)", size_subtract_null, "atomic != NULL"},
    {"tb_platform_atomic_size_load(NULL)", size_load_null, "atomic != NULL"},
    {"tb_platform_atomic_size_store(NULL, 1)", size_store_null, "atomic != NULL"},
    {"tb_platform_atomic_size_exchange(NULL, 1)", size_exchange_null, "atomic != NULL"},
    {"tb_platform_atomic_size_compare_exchange(NULL, &expected, 1)", size_compare_exchange_null,
     "atomic != NULL"},
    {"tb_platform_atomic_size_compare_exchange(&atomic, NULL, 1)",
     size_compare_exchange_null_expected, "expected != NULL"},
    {"tb_platform_atomic_u32_load(NULL)", u32_load_null, "atomic != NULL"},
    {"tb_platform_atomic_u32_store(NULL, 1)", u32_store_null, "atomic != NULL"},
    {"tb_platform_atomic_u32_exchange(NULL, 1)", u32_exchange_null, "atomic != NULL"},
    {"tb_platform_atomic_u32_compare_exchange(NULL, &expected, 1)", u32_compare_exchange_null,
     "atomic != NULL"},
    {"tb_platform_atomic_u32_compare_exchange(&atomic, NULL, 1)",
     u32_compare_exchange_null_expected, "expected != NULL"},
    {"tb_platform_atomic_pointer_load(NULL)", pointer_load_null, "atomic != NULL"},
    {"tb_platform_atomic_pointer_store(NULL, NULL)", pointer_store_null, "atomic != NULL"},
    {"tb_platform_atomic_pointer_exchange(NULL, NULL)", pointer_exchange_null, "atomic != NULL"},
    {"tb_platform_atomic_pointer_compare_exchange(NULL, &expected, NULL)",
     pointer_compare_exchange_null, "atomic != NULL"},
    {"tb_platform_atomic_pointer_compare_exchange(&atomic, NULL, NULL)",
     pointer_compare_exchange_null_expected, "expected != NULL"},
    {"tb_thread_create(NULL, entry, NULL)", create_null_thread, "thread != NULL"},
    {"tb_thread_create(&thread, NULL, NULL)", create_null_entry, "entry != NULL"},
    {"tb_thread_join(NULL)", join_null, "thread != NULL"},
    {"tb_mutex_lock(NULL)", lock_null, "mutex != NULL"},
    {"tb_mutex_try_lock(NULL)", try_lock_null, "mutex != NULL"},
    {"tb_mutex_unlock(NULL)", unlock_null, "mutex != NULL"},
    {"tb_condition_wait(NULL, &mutex)", wait_null_condition, "condition != NULL"},
    {"tb_condition_wait(&condition, NULL)", wait_null_mutex, "mutex != NULL"},
    {"tb_condition_wait_for(NULL, &mutex, 1)", wait_for_null_condition, "condition != NULL"},
    {"tb_condition_wait_for(&condition, NULL, 1)", wait_for_null_mutex, "mutex != NULL"},
    {"tb_condition_wake_one(NULL)", wake_one_null, "condition != NULL"},
    {"tb_condition_wake_all(NULL)", wake_all_null, "condition != NULL"},
};

TB_TEST(platform_contract, a_null_pointer_violates_the_contract_of_every_function)
{
    SKIP_WITHOUT_ASSERTIONS();
    for (size_t index = 0; index < TB_TEST_COUNT_OF(null_cases); index++)
    {
        TB_TEST_CONTEXT("%s", null_cases[index].call);
        TB_TEST_EXPECT_TRAP(null_cases[index].body, NULL, null_cases[index].assertion);
    }
}

/* ---------------------------------------------------------------------------
 * A thread that holds a mutex for the case.
 * ------------------------------------------------------------------------- */

typedef struct holder
{
    tb_mutex *held;       /* The mutex that the thread takes and keeps. */
    tb_mutex lock;        /* Guards the fields below. */
    tb_condition changed; /* Woken when a field below changes. */
    bool is_holding;      /* The thread has taken `held`. */
    bool may_let_go;      /* The thread may give `held` up and end. */
    bool has_given_up;    /* A wait reached its limit. */
    tb_thread thread;
} holder;

/* Waits until `*is_true`, which `self->lock` guards and the caller holds.
 * Returns false when a wait reached its limit instead. */
static bool holder_wait_until(holder *self, const bool *is_true)
{
    while (!*is_true)
    {
        if (!tb_condition_wait_for(&self->changed, &self->lock, PATIENCE_NANOSECONDS))
        {
            self->has_given_up = true;
            return false;
        }
    }
    return true;
}

static void hold_until_told(void *argument)
{
    holder *const self = argument;

    tb_mutex_lock(self->held);
    tb_mutex_lock(&self->lock);
    self->is_holding = true;
    tb_condition_wake_all(&self->changed);
    (void)holder_wait_until(self, &self->may_let_go);
    tb_mutex_unlock(&self->lock);
    tb_mutex_unlock(self->held);
}

/* Returns once another thread holds `mutex`, or false when it never did. */
static bool start_holding(holder *self, tb_mutex *mutex)
{
    bool is_holding = false;

    self->held = mutex;
    tb_thread_create(&self->thread, hold_until_told, self);
    tb_mutex_lock(&self->lock);
    is_holding = holder_wait_until(self, &self->is_holding);
    tb_mutex_unlock(&self->lock);
    return is_holding;
}

/* Lets the thread give the mutex up, and joins it. */
static void stop_holding(holder *self)
{
    tb_mutex_lock(&self->lock);
    self->may_let_go = true;
    tb_condition_wake_all(&self->changed);
    tb_mutex_unlock(&self->lock);
    tb_thread_join(&self->thread);
}

/* ---------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

static void unlock_it(void *context)
{
    tb_mutex_unlock(context);
}

static void lock_it(void *context)
{
    tb_mutex_lock(context);
}

TB_TEST(platform_contract, unlocking_a_mutex_that_nobody_holds_violates_the_contract)
{
    tb_mutex lock = {0};

    SKIP_WITHOUT_ASSERTIONS();
    /* Never locked, and locked and unlocked before. */
    TB_TEST_EXPECT_TRAP(unlock_it, &lock, "the calling thread holds the mutex");
    tb_mutex_lock(&lock);
    tb_mutex_unlock(&lock);
    TB_TEST_EXPECT_TRAP(unlock_it, &lock, "the calling thread holds the mutex");
    /* The refused calls changed nothing: the mutex still works. */
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
}

TB_TEST(platform_contract, unlocking_a_mutex_that_another_thread_holds_violates_the_contract)
{
    tb_mutex lock = {0};
    holder other = {0};
    const bool is_held = start_holding(&other, &lock);

    if (is_held && tb_assert_library_enabled())
    {
        TB_TEST_EXPECT_TRAP(unlock_it, &lock, "the calling thread holds the mutex");
        /* The other thread holds it still. */
        TB_TEST_EXPECT(!tb_mutex_try_lock(&lock));
    }
    stop_holding(&other);
    TB_TEST_REQUIRE(is_held);
    TB_TEST_EXPECT(!other.has_given_up);
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
}

TB_TEST(platform_contract, locking_a_mutex_twice_violates_the_contract)
{
    tb_mutex lock = {0};

    SKIP_WITHOUT_ASSERTIONS();
    tb_mutex_lock(&lock);
    TB_TEST_EXPECT_TRAP(lock_it, &lock, "the calling thread does not hold the mutex already");
    /* Held once, so one unlock frees it. */
    tb_mutex_unlock(&lock);
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    TB_TEST_EXPECT_TRAP(lock_it, &lock, "the calling thread does not hold the mutex already");
    tb_mutex_unlock(&lock);
}

/* Trying a mutex that the caller holds is allowed, and fails like any other
 * attempt on a held mutex. */
TB_TEST(platform_contract, trying_a_mutex_that_the_caller_holds_is_refused_without_a_violation)
{
    tb_mutex lock = {0};

    tb_mutex_lock(&lock);
    TB_TEST_EXPECT(!tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    TB_TEST_EXPECT(!tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
}

/* Holding one mutex says nothing about another. */
TB_TEST(platform_contract, the_holder_is_recorded_for_each_mutex_apart)
{
    tb_mutex first = {0};
    tb_mutex second = {0};

    SKIP_WITHOUT_ASSERTIONS();
    tb_mutex_lock(&first);
    TB_TEST_EXPECT_TRAP(unlock_it, &second, "the calling thread holds the mutex");
    tb_mutex_lock(&second);
    tb_mutex_unlock(&first);
    TB_TEST_EXPECT_TRAP(unlock_it, &first, "the calling thread holds the mutex");
    tb_mutex_unlock(&second);
}

/* ---------------------------------------------------------------------------
 * Condition variables.
 * ------------------------------------------------------------------------- */

typedef struct wait_arguments
{
    tb_condition *condition;
    tb_mutex *mutex;
    uint64_t nanoseconds;
} wait_arguments;

static void wait_on_it(void *context)
{
    const wait_arguments *const arguments = context;

    tb_condition_wait(arguments->condition, arguments->mutex);
}

static void wait_on_it_with_a_limit(void *context)
{
    const wait_arguments *const arguments = context;

    (void)tb_condition_wait_for(arguments->condition, arguments->mutex, arguments->nanoseconds);
}

TB_TEST(platform_contract, waiting_without_holding_the_mutex_violates_the_contract)
{
    tb_mutex lock = {0};
    tb_condition condition = {0};
    wait_arguments arguments = {.condition = &condition, .mutex = &lock, .nanoseconds = 1};

    SKIP_WITHOUT_ASSERTIONS();
    TB_TEST_EXPECT_TRAP(wait_on_it, &arguments, "the calling thread holds the mutex");
    TB_TEST_EXPECT_TRAP(wait_on_it_with_a_limit, &arguments, "the calling thread holds the mutex");
    /* A limit of zero returns at once, and is still a wait. */
    arguments.nanoseconds = 0;
    TB_TEST_EXPECT_TRAP(wait_on_it_with_a_limit, &arguments, "the calling thread holds the mutex");
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
}

TB_TEST(platform_contract, waiting_with_a_mutex_that_another_thread_holds_violates_the_contract)
{
    tb_mutex lock = {0};
    tb_condition condition = {0};
    wait_arguments arguments = {.condition = &condition, .mutex = &lock, .nanoseconds = 1};
    holder other = {0};
    const bool is_held = start_holding(&other, &lock);

    if (is_held && tb_assert_library_enabled())
    {
        TB_TEST_EXPECT_TRAP(wait_on_it, &arguments, "the calling thread holds the mutex");
        TB_TEST_EXPECT_TRAP(wait_on_it_with_a_limit, &arguments,
                            "the calling thread holds the mutex");
        TB_TEST_EXPECT(!tb_mutex_try_lock(&lock));
    }
    stop_holding(&other);
    TB_TEST_REQUIRE(is_held);
    TB_TEST_EXPECT(!other.has_given_up);
}

/* A wait gives the mutex up and takes it again, and the record of who holds
 * it has to follow: afterwards the waiter may unlock it, and may not lock it. */
TB_TEST(platform_contract, a_thread_holds_the_mutex_again_after_a_wait)
{
    tb_mutex lock = {0};
    tb_condition never_woken = {0};

    tb_mutex_lock(&lock);
    while (tb_condition_wait_for(&never_woken, &lock, 1))
    {
        /* Ended early with nobody to wake it, which a wait may do. */
    }
    TB_TEST_EXPECT(!tb_condition_wait_for(&never_woken, &lock, 0));
    if (tb_assert_library_enabled())
    {
        TB_TEST_EXPECT_TRAP(lock_it, &lock, "the calling thread does not hold the mutex already");
    }
    tb_mutex_unlock(&lock);
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    tb_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

static void join_it(void *context)
{
    tb_thread_join(context);
}

static void create_it_again(void *context)
{
    tb_thread_create(context, do_nothing, NULL);
}

TB_TEST(platform_contract, joining_a_thread_that_is_not_there_violates_the_contract)
{
    tb_thread thread = {0};

    SKIP_WITHOUT_ASSERTIONS();
    /* Never created. */
    TB_TEST_EXPECT_TRAP(join_it, &thread, "the thread was created and is not joined yet");
    /* Joined already. */
    tb_thread_create(&thread, do_nothing, NULL);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_TRAP(join_it, &thread, "the thread was created and is not joined yet");
}

typedef struct counted_run
{
    tb_platform_atomic_size run_count; /* Times the thread's function ran. */
} counted_run;

static void count_the_run(void *argument)
{
    counted_run *const counted = argument;

    (void)tb_platform_atomic_size_add(&counted->run_count, 1);
}

/* Creating over a thread that is not joined would lose it. The refused call
 * leaves the thread as it was, to be joined once. */
TB_TEST(platform_contract, creating_over_a_thread_that_is_not_joined_violates_the_contract)
{
    counted_run counted = {0};
    tb_thread thread = {0};

    tb_thread_create(&thread, count_the_run, &counted);
    if (tb_assert_library_enabled())
    {
        TB_TEST_EXPECT_TRAP(create_it_again, &thread,
                            "the thread object holds no thread that is not joined");
    }
    tb_thread_join(&thread);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&counted.run_count), ==, 1);
}
