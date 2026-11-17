/* A thread can be given a name, a stack size, and a priority when it is
 * created. These cases check what holds on every platform: the name is the
 * one the system then reports, cut where the system keeps less of it; the
 * stack is as large as was asked; and a thread runs whatever it was given.
 * What a priority becomes is each system's own, and is checked in the tests
 * of each adapter.
 *
 * A thread started by a case only records; the case checks after joining. */
#include "badger/assert.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

#define REPORT_CAPACITY 64

typedef struct name_report
{
    char name[REPORT_CAPACITY]; /* What the thread read as its own name. */
    size_t length;              /* What tb_thread_current_name() returned. */
    bool has_run;
} name_report;

static void report_own_name(void *argument)
{
    name_report *const report = argument;

    report->length = tb_thread_current_name(report->name, sizeof(report->name));
    report->has_run = true;
}

static size_t length_of(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

/* Starts a thread with `name`, and returns what it read as its own name. */
static name_report name_seen_by_a_thread_named(const char *name)
{
    const tb_thread_options options = {.name = name};
    name_report report = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, &options, report_own_name, &report);
    tb_thread_join(&thread);
    return report;
}

/* Fifteen bytes: the most that every supported system keeps. */
#define SHORT_NAME "badger-worker-3"

TB_TEST(platform_thread_options, a_thread_has_the_name_it_was_created_with)
{
    const name_report report = name_seen_by_a_thread_named(SHORT_NAME);

    TB_TEST_REQUIRE(report.has_run);
    TB_TEST_EXPECT_STRING(report.name, ==, SHORT_NAME);
    TB_TEST_EXPECT_UINT(report.length, ==, length_of(SHORT_NAME));
}

/* The bytes of a UTF-8 character after its first all have the form 10xxxxxx. */
static bool is_continuation(char byte)
{
    return ((unsigned char)byte & 0xC0u) == 0x80u;
}

/* Whether `kept` is what a system that keeps fewer bytes may make of `name`:
 * its beginning, cut between two characters, and not nothing. */
static bool is_a_whole_beginning_of(const char *kept, const char *name)
{
    size_t length = 0;

    while (kept[length] != '\0')
    {
        if (kept[length] != name[length])
        {
            return false;
        }
        length++;
    }
    return length > 0 && !is_continuation(name[length]);
}

/* Longer than the engine keeps. Every supported system keeps 15 bytes or
 * more, and none shows more than the engine kept. */
#define LONG_NAME "a-thread-name-that-is-longer-than-anything-keeps"

TB_TEST(platform_thread_options, a_long_name_is_cut_and_never_overruns)
{
    const name_report report = name_seen_by_a_thread_named(LONG_NAME);

    TB_TEST_REQUIRE(report.has_run);
    TB_TEST_CONTEXT("the thread read \"%s\"", report.name);
    TB_TEST_EXPECT(is_a_whole_beginning_of(report.name, LONG_NAME));
    TB_TEST_EXPECT_UINT(report.length, ==, length_of(report.name));
    TB_TEST_EXPECT_UINT(report.length, >=, 15);
    TB_TEST_EXPECT_UINT(report.length, <, TB_THREAD_NAME_CAPACITY);
}

/* Two-byte characters, so that a cut at 15 bytes and one at 31 both fall in
 * the middle of one: "é" sixteen times. */
#define ACCENTED_NAME                                                                              \
    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"                             \
    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"

TB_TEST(platform_thread_options, a_name_is_cut_between_characters)
{
    const name_report report = name_seen_by_a_thread_named(ACCENTED_NAME);

    TB_TEST_REQUIRE(report.has_run);
    TB_TEST_EXPECT(is_a_whole_beginning_of(report.name, ACCENTED_NAME));
    /* A whole number of two-byte characters. */
    TB_TEST_EXPECT_UINT(report.length % 2, ==, 0);
    TB_TEST_EXPECT_UINT(report.length, >=, 14);
}

/* A thread's name is its own: a later thread in the same object, and a
 * thread created beside it, do not inherit it. */
TB_TEST(platform_thread_options, each_thread_has_its_own_name)
{
    const tb_thread_options first_options = {.name = "first"};
    const tb_thread_options second_options = {.name = "second"};
    name_report first = {0};
    name_report second = {0};
    name_report reused = {0};
    tb_thread first_thread = {0};
    tb_thread second_thread = {0};

    tb_thread_create_with_options(&first_thread, &first_options, report_own_name, &first);
    tb_thread_create_with_options(&second_thread, &second_options, report_own_name, &second);
    tb_thread_join(&first_thread);
    tb_thread_join(&second_thread);
    /* The same object again, with another name. */
    tb_thread_create_with_options(&first_thread, &second_options, report_own_name, &reused);
    tb_thread_join(&first_thread);

    TB_TEST_EXPECT_STRING(first.name, ==, "first");
    TB_TEST_EXPECT_STRING(second.name, ==, "second");
    TB_TEST_EXPECT_STRING(reused.name, ==, "second");
}

/* The name is copied when the thread is created, so the caller's text may
 * change or go away while the thread runs. */
TB_TEST(platform_thread_options, the_name_is_copied_at_creation)
{
    char text[] = "copied";
    const tb_thread_options options = {.name = text};
    name_report report = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, &options, report_own_name, &report);
    /* The thread may not have named itself yet. */
    text[0] = 'X';
    text[1] = '\0';
    tb_thread_join(&thread);
    TB_TEST_EXPECT_STRING(report.name, ==, "copied");
}

TB_TEST(platform_thread_options, a_small_buffer_gets_the_beginning_of_the_name)
{
    char small[4] = {0};
    char one[1] = {'x'};
    char whole[REPORT_CAPACITY] = {0};
    const size_t whole_length = tb_thread_current_name(whole, sizeof(whole));
    const size_t small_length = tb_thread_current_name(small, sizeof(small));

    /* The thread of the case has whatever name the system gave it. */
    TB_TEST_EXPECT_UINT(whole_length, ==, length_of(whole));
    TB_TEST_EXPECT_UINT(small_length, ==, length_of(small));
    TB_TEST_EXPECT_UINT(small_length, <=, 3);
    TB_TEST_EXPECT_BYTES(small, whole, small_length);
    /* Room for the terminator alone. */
    TB_TEST_EXPECT_UINT(tb_thread_current_name(one, sizeof(one)), ==, 0);
    TB_TEST_EXPECT_UINT((unsigned char)one[0], ==, 0);
}

/* ---------------------------------------------------------------------------
 * No options, and options that ask for nothing.
 * ------------------------------------------------------------------------- */

typedef struct run_record
{
    void *argument_seen;
    bool has_run;
} run_record;

static void record_the_run(void *argument)
{
    run_record *const record = argument;

    record->argument_seen = argument;
    record->has_run = true;
}

TB_TEST(platform_thread_options, a_thread_runs_with_no_options_and_with_empty_ones)
{
    const tb_thread_options nothing = {0};
    const tb_thread_options empty_name = {.name = ""};
    const tb_thread_options *const each[] = {NULL, &nothing, &empty_name};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(each); index++)
    {
        run_record record = {0};
        tb_thread thread = {0};

        TB_TEST_CONTEXT("options %zu", index);
        tb_thread_create_with_options(&thread, each[index], record_the_run, &record);
        tb_thread_join(&thread);
        TB_TEST_EXPECT(record.has_run);
        TB_TEST_EXPECT_POINTER(record.argument_seen, ==, &record);
        /* A joined thread is zeroed, name and all, to be created again. */
        TB_TEST_EXPECT_UINT(thread.native, ==, 0);
        TB_TEST_EXPECT_UINT((unsigned char)thread.name[0], ==, 0);
    }
}

TB_TEST(platform_thread_options, a_thread_runs_at_every_priority)
{
    const tb_thread_priority priorities[] = {
        TB_THREAD_PRIORITY_NORMAL, TB_THREAD_PRIORITY_BACKGROUND, TB_THREAD_PRIORITY_HIGH};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(priorities); index++)
    {
        const tb_thread_options options = {.name = "prioritized", .priority = priorities[index]};
        run_record record = {0};
        tb_thread thread = {0};

        TB_TEST_CONTEXT("priority %zu", index);
        tb_thread_create_with_options(&thread, &options, record_the_run, &record);
        tb_thread_join(&thread);
        TB_TEST_EXPECT(record.has_run);
    }
}

/* ---------------------------------------------------------------------------
 * Stack sizes.
 * ------------------------------------------------------------------------- */

/* Larger than the default stack of any supported system, the largest of
 * which is 8 MiB: a thread that got a default one cannot use this much. */
#define LARGE_STACK_SIZE ((size_t)16 << 20)

/* How much of it the thread uses: three quarters, which leaves room for what
 * the system and a sanitizer keep on a stack. */
#define FRAME_SIZE ((size_t)16 << 10)
#define FRAME_COUNT ((size_t)768)

typedef struct stack_use
{
    uint64_t sum; /* Of one byte from every frame. */
    size_t depth_reached;
} stack_use;

/* Calls itself FRAME_COUNT deep, with FRAME_SIZE bytes of its own at every
 * level. Each frame is written before the call and read after it, so that no
 * compiler can drop a frame or turn the recursion into a loop. */
static uint64_t use_a_frame(size_t depth, stack_use *use)
{
    unsigned char frame[FRAME_SIZE] = {0};
    uint64_t deeper = 0;

    for (size_t index = 0; index < sizeof(frame); index += 256)
    {
        frame[index] = (unsigned char)(depth + index);
    }
    if (depth > use->depth_reached)
    {
        use->depth_reached = depth;
    }
    if (depth < FRAME_COUNT)
    {
        deeper = use_a_frame(depth + 1, use);
    }
    return deeper + frame[256];
}

static void use_the_stack(void *argument)
{
    stack_use *const use = argument;

    use->sum = use_a_frame(1, use);
}

TB_TEST(platform_thread_options, a_thread_can_use_the_stack_it_asked_for)
{
    const tb_thread_options options = {.name = "deep", .stack_size = LARGE_STACK_SIZE};
    stack_use use = {0};
    tb_thread thread = {0};
    uint64_t expected = 0;

    for (size_t depth = 1; depth <= FRAME_COUNT; depth++)
    {
        expected += (unsigned char)(depth + 256);
    }
    tb_thread_create_with_options(&thread, &options, use_the_stack, &use);
    tb_thread_join(&thread);
    TB_TEST_EXPECT_UINT(use.depth_reached, ==, FRAME_COUNT);
    TB_TEST_EXPECT_UINT(use.sum, ==, expected);
}

/* A size below the system's minimum, and one that is not a whole number of
 * pages, are raised to what the system accepts. */
TB_TEST(platform_thread_options, a_stack_size_the_system_cannot_use_is_rounded_up)
{
    const size_t sizes[] = {1, 4097, 65537, ((size_t)1 << 20) + 1};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        const tb_thread_options options = {.stack_size = sizes[index]};
        run_record record = {0};
        tb_thread thread = {0};

        TB_TEST_CONTEXT("stack size %zu", sizes[index]);
        tb_thread_create_with_options(&thread, &options, record_the_run, &record);
        tb_thread_join(&thread);
        TB_TEST_EXPECT(record.has_run);
    }
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void do_nothing(void *argument)
{
    (void)argument;
}

static void create_with_a_priority_that_is_none(void *context)
{
    tb_thread_options options = {0};
    tb_thread thread = {0};

    (void)context;
    options.priority = (tb_thread_priority)99;
    tb_thread_create_with_options(&thread, &options, do_nothing, NULL);
}

static void create_with_options_into_null(void *context)
{
    (void)context;
    tb_thread_create_with_options(NULL, NULL, do_nothing, NULL);
}

static void create_with_options_and_no_entry(void *context)
{
    tb_thread thread = {0};

    (void)context;
    tb_thread_create_with_options(&thread, NULL, NULL, NULL);
}

static void read_a_name_into_null(void *context)
{
    (void)context;
    (void)tb_thread_current_name(NULL, 8);
}

static void read_a_name_into_no_room(void *context)
{
    char buffer[1] = {0};

    (void)context;
    (void)tb_thread_current_name(buffer, 0);
}

TB_TEST(platform_thread_options, bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_with_a_priority_that_is_none, NULL,
                        "the priority is one of tb_thread_priority");
    TB_TEST_EXPECT_TRAP(create_with_options_into_null, NULL, "thread != NULL");
    TB_TEST_EXPECT_TRAP(create_with_options_and_no_entry, NULL, "entry != NULL");
    TB_TEST_EXPECT_TRAP(read_a_name_into_null, NULL, "buffer != NULL");
    TB_TEST_EXPECT_TRAP(read_a_name_into_no_room, NULL, "capacity >= 1");
}
