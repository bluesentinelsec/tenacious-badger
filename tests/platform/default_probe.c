/* Calls each function of the default platform adapter, the one a platform
 * without an adapter of its own is built with. Every function must report
 * "not implemented" and end the process, so each case here is expected to
 * end the process: ExpectPlatformDefault.cmake runs each as a child process and
 * checks how it ends. They are not registered with CTest directly.
 *
 * A case is named after the function it calls. The script reads the public
 * headers for the functions, so a function that is added to a platform
 * interface without a case here fails the test. */
#include "badger/platform.h"
#include "badger/system.h"
#include "badger/thread.h"

#include "tb_test.h"

static void do_nothing(void *argument)
{
    (void)argument;
}

TB_TEST(platform_default, tb_platform_abort)
{
    tb_platform_abort();
}

TB_TEST(platform_default, tb_platform_clock_nanoseconds)
{
    (void)tb_platform_clock_nanoseconds();
}

TB_TEST(platform_default, tb_platform_atomic_size_add)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_add(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_subtract)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_subtract(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_size_load)
{
    tb_platform_atomic_size atomic = {0};

    (void)tb_platform_atomic_size_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_u32_load)
{
    tb_platform_atomic_u32 atomic = {0};

    (void)tb_platform_atomic_u32_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_u32_store)
{
    tb_platform_atomic_u32 atomic = {0};

    tb_platform_atomic_u32_store(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_u32_exchange)
{
    tb_platform_atomic_u32 atomic = {0};

    (void)tb_platform_atomic_u32_exchange(&atomic, 1);
}

TB_TEST(platform_default, tb_platform_atomic_u32_compare_exchange)
{
    tb_platform_atomic_u32 atomic = {0};
    uint32_t expected = 0;

    (void)tb_platform_atomic_u32_compare_exchange(&atomic, &expected, 1);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_load)
{
    tb_platform_atomic_pointer atomic = {0};

    (void)tb_platform_atomic_pointer_load(&atomic);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_store)
{
    tb_platform_atomic_pointer atomic = {0};

    tb_platform_atomic_pointer_store(&atomic, NULL);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_exchange)
{
    tb_platform_atomic_pointer atomic = {0};

    (void)tb_platform_atomic_pointer_exchange(&atomic, NULL);
}

TB_TEST(platform_default, tb_platform_atomic_pointer_compare_exchange)
{
    tb_platform_atomic_pointer atomic = {0};
    void *expected = NULL;

    (void)tb_platform_atomic_pointer_compare_exchange(&atomic, &expected, NULL);
}

TB_TEST(platform_default, tb_system_get_processors)
{
    tb_system_processors processors = {0};

    tb_system_get_processors(&processors);
}

TB_TEST(platform_default, tb_thread_create)
{
    tb_thread thread = {0};

    tb_thread_create(&thread, do_nothing, NULL);
}

TB_TEST(platform_default, tb_thread_join)
{
    tb_thread thread = {0};

    tb_thread_join(&thread);
}

TB_TEST(platform_default, tb_thread_current_id)
{
    (void)tb_thread_current_id();
}

TB_TEST(platform_default, tb_mutex_lock)
{
    tb_mutex mutex = {0};

    tb_mutex_lock(&mutex);
}

TB_TEST(platform_default, tb_mutex_try_lock)
{
    tb_mutex mutex = {0};

    (void)tb_mutex_try_lock(&mutex);
}

TB_TEST(platform_default, tb_mutex_unlock)
{
    tb_mutex mutex = {0};

    tb_mutex_unlock(&mutex);
}

TB_TEST(platform_default, tb_condition_wait)
{
    tb_condition condition = {0};
    tb_mutex mutex = {0};

    tb_condition_wait(&condition, &mutex);
}

TB_TEST(platform_default, tb_condition_wait_for)
{
    tb_condition condition = {0};
    tb_mutex mutex = {0};

    (void)tb_condition_wait_for(&condition, &mutex, 1);
}

TB_TEST(platform_default, tb_condition_wake_one)
{
    tb_condition condition = {0};

    tb_condition_wake_one(&condition);
}

TB_TEST(platform_default, tb_condition_wake_all)
{
    tb_condition condition = {0};

    tb_condition_wake_all(&condition);
}
