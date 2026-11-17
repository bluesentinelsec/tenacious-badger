#include "badger/thread.h"

#include "not_implemented.h"

/* A lock that did nothing would let every thread in, and a thread that ran
 * its function on the caller's would hide that there are no threads. Each
 * function reports that it is missing. */

#define TB_DEFAULT_THREAD_INTERFACE "platform.thread"

void tb_thread_create(tb_thread *thread, tb_thread_function *entry, void *argument)
{
    (void)thread;
    (void)entry;
    (void)argument;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_create");
}

void tb_thread_create_with_options(tb_thread *thread, const tb_thread_options *options,
                                   tb_thread_function *entry, void *argument)
{
    (void)thread;
    (void)options;
    (void)entry;
    (void)argument;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_create_with_options");
}

void tb_thread_join(tb_thread *thread)
{
    (void)thread;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_join");
}

uint64_t tb_thread_current_id(void)
{
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_current_id");
}

size_t tb_thread_current_name(char *buffer, size_t capacity)
{
    (void)buffer;
    (void)capacity;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_current_name");
}

void tb_mutex_lock(tb_mutex *mutex)
{
    (void)mutex;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_mutex_lock");
}

bool tb_mutex_try_lock(tb_mutex *mutex)
{
    (void)mutex;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_mutex_try_lock");
}

void tb_mutex_unlock(tb_mutex *mutex)
{
    (void)mutex;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_mutex_unlock");
}

void tb_condition_wait(tb_condition *condition, tb_mutex *mutex)
{
    (void)condition;
    (void)mutex;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_condition_wait");
}

bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds)
{
    (void)condition;
    (void)mutex;
    (void)nanoseconds;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_condition_wait_for");
}

void tb_condition_wake_one(tb_condition *condition)
{
    (void)condition;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_condition_wake_one");
}

void tb_condition_wake_all(tb_condition *condition)
{
    (void)condition;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_condition_wake_all");
}

void tb_semaphore_signal(tb_semaphore *semaphore, size_t count)
{
    (void)semaphore;
    (void)count;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_semaphore_signal");
}

void tb_semaphore_wait(tb_semaphore *semaphore)
{
    (void)semaphore;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_semaphore_wait");
}

bool tb_semaphore_try_wait(tb_semaphore *semaphore)
{
    (void)semaphore;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_semaphore_try_wait");
}

bool tb_semaphore_wait_for(tb_semaphore *semaphore, uint64_t nanoseconds)
{
    (void)semaphore;
    (void)nanoseconds;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_semaphore_wait_for");
}

void tb_thread_local_create(tb_thread_local *local, tb_thread_local_destructor *destructor)
{
    (void)local;
    (void)destructor;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_local_create");
}

void tb_thread_local_destroy(tb_thread_local *local)
{
    (void)local;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_local_destroy");
}

void *tb_thread_local_get(const tb_thread_local *local)
{
    (void)local;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_local_get");
}

void tb_thread_local_set(const tb_thread_local *local, void *value)
{
    (void)local;
    (void)value;
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_local_set");
}

void tb_thread_relax(void)
{
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_relax");
}

void tb_thread_yield(void)
{
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_yield");
}

bool tb_thread_can_be_confined(void)
{
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_can_be_confined");
}

size_t tb_thread_current_processor(void)
{
    tb_default_not_implemented(TB_DEFAULT_THREAD_INTERFACE, "tb_thread_current_processor");
}
