/* A captured stack is a list of addresses, and the claim to check is that
 * they are the right ones: the first is in the function that asked, the next
 * in the function that called it, and so on outward. Four functions call one
 * another here, and each address must belong to the function it should.
 *
 * Which function an address belongs to is decided in two ways, because
 * neither works everywhere. A platform that can name the function is asked
 * for the name. One that cannot is checked by where the address lies: it
 * must follow the start of the right function more closely than the start of
 * any of the others. */
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/stack.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FRAME_CAPACITY 24

typedef struct capture
{
    void *frames[FRAME_CAPACITY];
    size_t capacity; /* How many frames to ask for. */
    size_t skip;     /* How many to have left out. */
    size_t count;    /* How many were written. */
    size_t returns;  /* Raised by each function after the one it called returns. */
} capture;

/* Not static, so that a platform which names only what a program keeps the
 * names of can name them. */
void stack_test_innermost(capture *into);
void stack_test_middle(capture *into);
void stack_test_outer(capture *into);
void stack_test_outermost(capture *into);

/* Each call goes through a pointer that the compiler must read again every
 * time, and each function has something left to do when its call returns.
 * So no function is joined into its caller, and none is left by a jump where
 * the source has a call: each has a frame of its own in every build. */
static void (*volatile call_innermost)(capture *) = stack_test_innermost;
static void (*volatile call_middle)(capture *) = stack_test_middle;
static void (*volatile call_outer)(capture *) = stack_test_outer;
static void (*volatile call_outermost)(capture *) = stack_test_outermost;

void stack_test_innermost(capture *into)
{
    into->count = tb_stack_capture(into->frames, into->capacity, into->skip);
    into->returns++;
}

void stack_test_middle(capture *into)
{
    call_innermost(into);
    into->returns++;
}

void stack_test_outer(capture *into)
{
    call_middle(into);
    into->returns++;
}

void stack_test_outermost(capture *into)
{
    call_outer(into);
    into->returns++;
}

typedef void test_function(capture *into);

#define FUNCTION_COUNT 4

static test_function *const functions[FUNCTION_COUNT] = {stack_test_innermost, stack_test_middle,
                                                         stack_test_outer, stack_test_outermost};
static const char *const names[FUNCTION_COUNT] = {"stack_test_innermost", "stack_test_middle",
                                                  "stack_test_outer", "stack_test_outermost"};

/* Bytes from the start of a function within which an address is taken to be
 * part of it. The four are a few instructions each. */
#define FUNCTION_SIZE_MAX ((uintptr_t)2048)

/* Which of the four functions an address belongs to, or FUNCTION_COUNT for
 * none. Told by the address alone, "none" cannot be relied on: other code
 * may follow the last of them closely. */
static size_t owner_by_address(const void *address)
{
    const uintptr_t value = (uintptr_t)address;
    uintptr_t nearest = FUNCTION_SIZE_MAX;
    size_t owner = TB_TEST_COUNT_OF(functions);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(functions); index++)
    {
        const uintptr_t start = (uintptr_t)functions[index];

        if (value > start && value - start < nearest)
        {
            nearest = value - start;
            owner = index;
        }
    }
    return owner;
}

static size_t owner_of(const void *address)
{
    tb_stack_location location = {0};

    tb_stack_describe(address, &location);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        if (strcmp(location.function, names[index]) == 0)
        {
            return index;
        }
    }
    /* Not named, or named by something that knows only other functions. */
    return owner_by_address(address);
}

static capture capture_with(size_t capacity, size_t skip)
{
    capture into = {.capacity = capacity, .skip = skip};

    call_outermost(&into);
    return into;
}

TB_TEST(platform_stack, the_frames_are_the_callers_from_the_innermost_outward)
{
    const capture all = capture_with(FRAME_CAPACITY, 0);

    TB_TEST_EXPECT_UINT(all.returns, ==, FUNCTION_COUNT);
    /* The four functions, and at least the case that called them. */
    TB_TEST_REQUIRE_UINT(all.count, >, FUNCTION_COUNT);
    TB_TEST_REQUIRE_UINT(all.count, <=, FRAME_CAPACITY);
    for (size_t index = 0; index < FUNCTION_COUNT; index++)
    {
        TB_TEST_CONTEXT("frame %zu", index);
        TB_TEST_EXPECT_UINT(owner_of(all.frames[index]), ==, index);
    }
    for (size_t index = 0; index < all.count; index++)
    {
        TB_TEST_EXPECT_POINTER(all.frames[index], !=, NULL);
    }
}

TB_TEST(platform_stack, frames_can_be_left_out_and_limited)
{
    const capture all = capture_with(FRAME_CAPACITY, 0);

    for (size_t skip = 1; skip < FUNCTION_COUNT; skip++)
    {
        const capture some = capture_with(FRAME_CAPACITY, skip);

        TB_TEST_CONTEXT("leaving out %zu", skip);
        TB_TEST_REQUIRE_UINT(some.count + skip, ==, all.count);
        /* What is left is what the whole stack had after the ones left out,
         * as far as the four functions go. Beyond them is this case, which
         * called them from another place the second time. */
        TB_TEST_EXPECT_UINT(owner_of(some.frames[0]), ==, skip);
        for (size_t index = 0; index + skip < FUNCTION_COUNT; index++)
        {
            TB_TEST_EXPECT_POINTER(some.frames[index], ==, all.frames[index + skip]);
        }
    }
    for (size_t capacity = 0; capacity <= FUNCTION_COUNT; capacity++)
    {
        const capture some = capture_with(capacity, 0);

        TB_TEST_CONTEXT("room for %zu", capacity);
        TB_TEST_EXPECT_UINT(some.count, ==, capacity);
        /* Nothing was written past the room there was. */
        TB_TEST_EXPECT_POINTER(some.frames[capacity], ==, NULL);
    }
    /* More left out than any stack is deep. */
    TB_TEST_EXPECT_UINT(capture_with(FRAME_CAPACITY, all.count).count, ==, 0);
    TB_TEST_EXPECT_UINT(capture_with(FRAME_CAPACITY, SIZE_MAX).count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_stack_capture(NULL, 0, 0), ==, 0);
}

TB_TEST(platform_stack, an_address_is_described_by_its_module)
{
    const capture all = capture_with(FRAME_CAPACITY, 0);
    tb_stack_location location = {0};
    tb_stack_location direct = {0};
    tb_stack_location nothing = {.line = 7, .function = "left over", .module = "left over"};
    /* An address in this program that no capture is needed for. In a build
     * that links incrementally it is a jump to the function, not the
     * function, so only its module is checked. */
    const void *const code = (const void *)(uintptr_t)stack_test_innermost;

    tb_stack_describe(code, &direct);
    TB_TEST_EXPECT_POINTER(direct.address, ==, code);
    TB_TEST_EXPECT(strncmp(direct.module, "tb_platform_tests", 17) == 0);
    TB_TEST_EXPECT_UINT(direct.module_offset, >, 0);

    TB_TEST_REQUIRE_UINT(all.count, >=, 3);
    tb_stack_describe(all.frames[0], &location);
    TB_TEST_EXPECT_POINTER(location.address, ==, all.frames[0]);
    /* The program that this case is part of, whatever else is known. */
    TB_TEST_EXPECT(strncmp(location.module, "tb_platform_tests", 17) == 0);
    TB_TEST_EXPECT_UINT(location.module_offset, >, 0);
    /* What is known is right, and what is not is empty and not a guess. */
    if (location.function[0] != '\0')
    {
        TB_TEST_EXPECT_STRING(location.function, ==, "stack_test_innermost");
    }
    if (location.file[0] != '\0')
    {
        const size_t length = strlen(location.file);

        TB_TEST_REQUIRE_UINT(length, >=, 12);
        TB_TEST_EXPECT_STRING(location.file + length - 12, ==, "stack_test.c");
        TB_TEST_EXPECT_UINT(location.line, >, 0);
    }
    else
    {
        TB_TEST_EXPECT_UINT(location.line, ==, 0);
    }

    /* No address at all: every field is cleared. */
    tb_stack_describe(NULL, &nothing);
    TB_TEST_EXPECT_POINTER(nothing.address, ==, NULL);
    TB_TEST_EXPECT_STRING(nothing.function, ==, "");
    TB_TEST_EXPECT_STRING(nothing.module, ==, "");
    TB_TEST_EXPECT_STRING(nothing.file, ==, "");
    TB_TEST_EXPECT_UINT(nothing.line, ==, 0);
    TB_TEST_EXPECT_UINT(nothing.function_offset, ==, 0);
    TB_TEST_EXPECT_UINT(nothing.module_offset, ==, 0);
}

#define THREAD_COUNT 8
#define THREAD_ROUNDS ((size_t)200)

static void capture_and_describe(void *argument)
{
    tb_platform_atomic_size *const wrong_count = argument;

    for (size_t round = 0; round < THREAD_ROUNDS; round++)
    {
        const capture all = capture_with(FRAME_CAPACITY, 0);
        tb_stack_location location = {0};

        tb_stack_describe(all.frames[round % FUNCTION_COUNT], &location);
        if (all.count <= FUNCTION_COUNT || owner_of(all.frames[0]) != 0 ||
            owner_of(all.frames[3]) != 3 || strncmp(location.module, "tb_platform_tests", 17) != 0)
        {
            (void)tb_platform_atomic_size_add(wrong_count, 1);
        }
    }
}

/* Every thread has a stack of its own, and the system's library that names
 * addresses, which on Windows may be used by one thread at a time, is shared
 * by all of them. */
TB_TEST(platform_stack, threads_capture_and_describe_at_the_same_time)
{
    tb_platform_atomic_size wrong_count = {0};
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], capture_and_describe, &wrong_count);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&wrong_count), ==, 0);
}

static void capture_into_null(void *context)
{
    (void)context;
    (void)tb_stack_capture(NULL, 4, 0);
}

static void describe_into_null(void *context)
{
    tb_stack_describe(context, NULL);
}

TB_TEST(platform_stack, missing_arguments_violate_the_contract)
{
    int something = 0;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(capture_into_null, NULL, "frames != NULL");
    TB_TEST_EXPECT_TRAP(describe_into_null, &something, "location != NULL");
}
