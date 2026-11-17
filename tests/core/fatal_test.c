/* Every condition that ends the process is reported in one place. The cases
 * put a handler in that place and check what arrives: the message as it was
 * formatted, the call stack of whoever met the condition, and, from the
 * modules that have a handler of their own, their report when that handler is
 * not installed.
 *
 * What the default does with a report, which is to write it and end the
 * process, is checked in a child process by ExpectFatalAbort.cmake. */
#include "badger/fatal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/memory_debug.h"
#include "badger/stack.h"
#include "badger/str.h"
#include "engine_test.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

typedef struct received
{
    char message[TB_FATAL_MESSAGE_CAPACITY * 2]; /* Room to see one that is too long. */
    void *frames[TB_FATAL_FRAMES_MAX];
    size_t frame_count;
    size_t count; /* Reports so far. */
} received;

static received last = {0};

/* Keeps the report and leaves by a trap, as a handler in a test may. */
static void keep_and_trap(const tb_fatal_report *report, void *context)
{
    received *const into = context;

    into->count++;
    into->frame_count = report->frame_count;
    for (size_t index = 0; index < report->frame_count && index < TB_FATAL_FRAMES_MAX; index++)
    {
        into->frames[index] = report->frames[index];
    }
    memset(into->message, 0, sizeof(into->message));
    memcpy(into->message, report->message, tb_str_length(report->message));
    tb_test_trap(__FILE__, __LINE__, report->message);
}

static void start_receiving(void)
{
    memset(&last, 0, sizeof(last));
    tb_fatal_set_handler(keep_and_trap, &last);
}

static void stop_receiving(void)
{
    tb_fatal_set_handler(NULL, NULL);
}

static void fail_with_arguments(void *context)
{
    (void)context;
    tb_fatal("render: %s failed with %d of %zu", "the loader", -7, (size_t)12);
}

static void fail_with_a_string(void *context)
{
    tb_fatal_message(context);
}

TB_TEST(core_fatal, a_fatal_condition_reaches_the_handler_as_it_was_formatted)
{
    char with_newlines[] = "audio: the device was lost\n\n";
    char with_percent[] = "jobs: 100%s of the slots are taken";

    start_receiving();
    TB_TEST_EXPECT_TRAP(fail_with_arguments, NULL, "render: the loader failed with -7 of 12");
    TB_TEST_EXPECT_STRING(last.message, ==, "render: the loader failed with -7 of 12");
    TB_TEST_EXPECT_UINT(last.count, ==, 1);

    /* A message that is already a string is taken as it is, percent signs
     * included, and loses one newline from its end. */
    TB_TEST_EXPECT_TRAP(fail_with_a_string, with_percent, "100%s");
    TB_TEST_EXPECT_STRING(last.message, ==, "jobs: 100%s of the slots are taken");
    TB_TEST_EXPECT_TRAP(fail_with_a_string, with_newlines, "the device was lost");
    TB_TEST_EXPECT_STRING(last.message, ==, "audio: the device was lost\n");
    TB_TEST_EXPECT_UINT(last.count, ==, 3);
    stop_receiving();
}

static void fail_with_a_long_message(void *context)
{
    tb_fatal("%s", (const char *)context);
}

TB_TEST(core_fatal, a_message_that_is_too_long_is_cut_off)
{
    char text[TB_FATAL_MESSAGE_CAPACITY + 200] = {0};

    memset(text, 'x', sizeof(text) - 1);
    start_receiving();
    TB_TEST_EXPECT_TRAP(fail_with_a_long_message, text, "xxxx");
    TB_TEST_EXPECT_UINT(tb_str_length(last.message), ==, TB_FATAL_MESSAGE_CAPACITY - 1);
    TB_TEST_EXPECT_TRAP(fail_with_a_string, text, "xxxx");
    TB_TEST_EXPECT_UINT(tb_str_length(last.message), ==, TB_FATAL_MESSAGE_CAPACITY - 1);
    stop_receiving();
}

/* Not static, so that a platform which names only what a program keeps the
 * names of can name it. */
void fatal_test_meet_a_condition(void *context);

void fatal_test_meet_a_condition(void *context)
{
    (void)context;
    tb_fatal("test: a condition");
}

/* The report carries the stack of whoever met the condition: among its first
 * frames is the function that called tb_fatal(), wherever a platform can name
 * it, and every frame is an address in some module. */
TB_TEST(core_fatal, the_report_carries_the_stack_that_led_to_it)
{
    bool is_named = false;
    bool is_nameable = false;

    start_receiving();
    TB_TEST_EXPECT_TRAP(fatal_test_meet_a_condition, NULL, "test: a condition");
    stop_receiving();
    TB_TEST_REQUIRE_UINT(last.frame_count, >=, 2);
    TB_TEST_REQUIRE_UINT(last.frame_count, <=, TB_FATAL_FRAMES_MAX);
    for (size_t index = 0; index < last.frame_count; index++)
    {
        tb_stack_location location = {0};

        TB_TEST_REQUIRE_POINTER(last.frames[index], !=, NULL);
        tb_stack_describe(last.frames[index], &location);
        if (index < 3 && tb_str_equal(location.function, "fatal_test_meet_a_condition"))
        {
            is_named = true;
        }
        if (index < 3 && location.function[0] != '\0')
        {
            is_nameable = true;
        }
    }
    /* Where the platform names any of the innermost functions, it names the
     * one that met the condition. */
    if (is_nameable)
    {
        tb_stack_location first = {0};

        tb_stack_describe(last.frames[0], &first);
        /* The first frame is tb_fatal itself, unless the build joined it into
         * its caller. */
        TB_TEST_EXPECT(tb_str_equal(first.function, "tb_fatal") || is_named);
    }
}

static void fail_an_assertion(void *context)
{
    (void)context;
    tb_assert_fail("count > 0", "there must be one", "probe.c", 12, "probe_function");
}

static void fail_an_assertion_without_a_message(void *context)
{
    (void)context;
    tb_assert_fail("count > 0", NULL, "probe.c", 12, "probe_function");
}

static void run_out_of_memory(void *context)
{
    (void)context;
    (void)tb_calloc(3, 64);
}

static void free_what_is_no_block(void *context)
{
    tb_memory_functions checking = {0};

    tb_memory_debug_get_functions(&checking);
    checking.free_function(context);
}

/* A failed assertion, exhausted memory, and a memory fault each have a
 * handler of their own. With none installed, each is a fatal condition like
 * any other, and says the same as it always did. */
TB_TEST(core_fatal, the_reports_of_other_modules_arrive_when_they_have_no_handler)
{
    int no_block = 0;

    start_receiving();

    tb_assert_set_handler(NULL, NULL);
    TB_TEST_EXPECT_TRAP(fail_an_assertion, NULL, "assertion failed");
    TB_TEST_EXPECT_STRING(
        last.message, ==,
        "probe.c:12: assertion failed in probe_function: count > 0 (there must be one)");
    TB_TEST_EXPECT_TRAP(fail_an_assertion_without_a_message, NULL, "assertion failed");
    TB_TEST_EXPECT_STRING(last.message, ==,
                          "probe.c:12: assertion failed in probe_function: count > 0");
    tb_engine_test_install_assert_handler();

    tb_memory_set_failure_handler(NULL, NULL);
    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_TRAP(run_out_of_memory, NULL, "out of memory");
    tb_engine_test_stop_failing_allocations();
    tb_engine_test_install_memory_failure_handler();
    TB_TEST_EXPECT_STRING(last.message, ==,
                          "badger: out of memory allocating 3 x 64 bytes (alignment 0)");

    if (!tb_engine_test_is_checking_memory())
    {
        tb_memory_debug_set_handler(NULL, NULL);
        TB_TEST_EXPECT_TRAP(free_what_is_no_block, &no_block, "is not an allocated block");
        TB_TEST_EXPECT(tb_str_starts_with(last.message, "badger: memory fault: "));
        TB_TEST_EXPECT_UINT(last.count, ==, 4);
    }
    TB_TEST_EXPECT_UINT(last.frame_count, >, 0);
    stop_receiving();
}

static void fail_with_no_format(void *context)
{
    (void)context;
    tb_fatal(NULL);
}

static void fail_with_no_message(void *context)
{
    (void)context;
    tb_fatal_message(NULL);
}

static void write_no_report(void *context)
{
    (void)context;
    tb_fatal_write(NULL);
}

TB_TEST(core_fatal, missing_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(fail_with_no_format, NULL, "format != NULL");
    TB_TEST_EXPECT_TRAP(fail_with_no_message, NULL, "message != NULL");
    TB_TEST_EXPECT_TRAP(write_no_report, NULL, "report != NULL");
}
