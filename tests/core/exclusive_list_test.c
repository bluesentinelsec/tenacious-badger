/* A list checks, in builds with assertions, that one thread at a time
 * changes it and that no thread reads it while another changes it, like the
 * containers that tests/core/exclusive_test.c covers. A list allocates
 * nothing, so no allocator can hold a thread inside one of its functions.
 * Here a thread enters the list's guard itself, which the engine's private
 * header allows, and stays there while the test calls the list. */
#include "badger/assert.h"
#include "badger/list.h"
#include "badger/thread.h"
#include "core/exclusive.h"
#include "tb_test.h"

typedef struct list_holder
{
    tb_list *list;
    tb_semaphore inside;  /* Signalled once the thread is inside the list. */
    tb_semaphore release; /* Signalled to let it leave. */
} list_holder;

static void change_the_list_until_released(void *context)
{
    list_holder *const holder = context;
    TB_EXCLUSIVE_ENTER(&holder->list->check, "a list");
    tb_semaphore_signal(&holder->inside, 1);
    tb_semaphore_wait(&holder->release);
    TB_EXCLUSIVE_LEAVE(&holder->list->check);
}

typedef struct list_call
{
    tb_list *list;
    tb_list *other;
    tb_list_link *link;
} list_call;

static void push_back(void *context)
{
    const list_call *const call = context;
    tb_list_push_back(call->list, call->link);
}
static void pop_front(void *context)
{
    const list_call *const call = context;
    (void)tb_list_pop_front(call->list);
}
static void append(void *context)
{
    const list_call *const call = context;
    tb_list_append(call->other, call->list);
}
static void reverse(void *context)
{
    const list_call *const call = context;
    tb_list_reverse(call->list);
}
static void first(void *context)
{
    const list_call *const call = context;
    (void)tb_list_first(call->list);
}
static void is_empty(void *context)
{
    const list_call *const call = context;
    (void)tb_list_is_empty(call->list);
}
static void length(void *context)
{
    const list_call *const call = context;
    (void)tb_list_length(call->list);
}

/* Every function that names the list fails while another thread is changing
 * it: those that change it as two changes do, those that read it as a read
 * during a change does. Once that thread has left, the list is as it was. */
TB_TEST(core_exclusive_list, calls_while_another_thread_changes_the_list_fail_the_check)
{
    static const struct
    {
        const char *name;
        void (*call)(void *context);
        const char *message;
    } calls[] = {
        {"push_back", push_back, TB_EXCLUSIVE_WRITE_WRITE_MESSAGE},
        {"pop_front", pop_front, TB_EXCLUSIVE_WRITE_WRITE_MESSAGE},
        {"append", append, TB_EXCLUSIVE_WRITE_WRITE_MESSAGE},
        {"reverse", reverse, TB_EXCLUSIVE_WRITE_WRITE_MESSAGE},
        {"first", first, TB_EXCLUSIVE_READ_WRITE_MESSAGE},
        {"is_empty", is_empty, TB_EXCLUSIVE_READ_WRITE_MESSAGE},
        {"length", length, TB_EXCLUSIVE_READ_WRITE_MESSAGE},
    };
    tb_list list = TB_LIST_INIT(list);
    tb_list other = TB_LIST_INIT(other);
    tb_list_link member = {0};
    tb_list_link newcomer = {0};
    list_holder holder = {0};
    list_call call = {&list, &other, &newcomer};
    tb_thread thread = {0};
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    holder.list = &list;
    tb_list_push_back(&list, &member);
    tb_thread_create(&thread, change_the_list_until_released, &holder);
    tb_semaphore_wait(&holder.inside);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(calls); index++)
    {
        TB_TEST_CONTEXT("%s", calls[index].name);
        TB_TEST_EXPECT_TRAP(calls[index].call, &call, calls[index].message);
    }
    tb_semaphore_signal(&holder.release, 1);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_POINTER(tb_list_first(&list), ==, &member);
    TB_TEST_EXPECT_UINT(tb_list_length(&list), ==, 1);
    push_back(&call);
    TB_TEST_EXPECT_UINT(tb_list_length(&list), ==, 2);
    tb_list_remove(&newcomer);
    tb_list_remove(&member);
}

typedef struct list_reading
{
    tb_list *list;
    size_t generation;
} list_reading;

static void push_on_another_thread(void *context)
{
    static tb_list_link pushed = {0};
    tb_list_push_back(context, &pushed);
}

static void end_reading(void *context)
{
    const list_reading *const reading = context;
    TB_EXCLUSIVE_READ_LEAVE(&reading->list->check, reading->generation, "a list");
}

/* A change that another thread makes from start to end while the test's
 * thread is reading the list fails the check when the read ends. */
TB_TEST(core_exclusive_list, a_change_during_a_read_fails_the_check_when_the_read_ends)
{
    tb_list list = TB_LIST_INIT(list);
    list_reading reading = {&list, 0};
    tb_thread thread = {0};
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    reading.generation = TB_EXCLUSIVE_READ_ENTER(&list.check, "a list");
    tb_thread_create(&thread, push_on_another_thread, &list);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_TRAP(end_reading, &reading, TB_EXCLUSIVE_READ_WRITE_MESSAGE);
    /* Without a change in between, a read ends cleanly. */
    reading.generation = TB_EXCLUSIVE_READ_ENTER(&list.check, "a list");
    end_reading(&reading);
    tb_list_remove(tb_list_first(&list));
}

/* The same thread may read a list it is changing, and a list emptied by
 * tb_list_init() starts with no thread inside, whatever its memory held. */
TB_TEST(core_exclusive_list, one_thread_reads_what_it_changes_and_init_clears_the_check)
{
    tb_list list = {0};
    tb_list other = TB_LIST_INIT(other);
    tb_list_link member = {0};
    list.check.depth = 7;
    list.check.owner.value = 1;
    tb_list_init(&list);
    tb_list_push_back(&other, &member);
    /* Appending reads the other list while changing both. */
    tb_list_append(&list, &other);
    TB_TEST_EXPECT(tb_list_is_empty(&other));
    TB_TEST_EXPECT_POINTER(tb_list_pop_front(&list), ==, &member);
    TB_TEST_EXPECT(tb_list_is_empty(&list));
}
