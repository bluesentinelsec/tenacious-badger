#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/thread.h"
#include "core/arena_internal.h"
#include "tb_test.h"
#include <string.h>

static size_t commit_calls = {0};
static size_t fail_commit = {0};
static size_t live_reservations = {0};
static bool refuse_reservation = {0};
static bool refuse_decommit = {0};
static tb_vm_result reserve_memory(size_t size, tb_vm_region *region)
{
    tb_vm_result result = {0};
    if (refuse_reservation)
    {
        return TB_VM_NO_MEMORY;
    }
    result = tb_vm_reserve(size, region);
    if (result == TB_VM_OK)
    {
        live_reservations++;
    }
    return result;
}
/* Once armed, the next commit holds its thread until the test lets it go. */
static tb_platform_atomic_u32 hold_commit = {0};
static tb_semaphore commit_inside = {0};
static tb_semaphore commit_release = {0};
static tb_vm_result commit_memory(tb_vm_region *region, size_t offset, size_t size)
{
    if (tb_platform_atomic_u32_exchange(&hold_commit, 0) != 0)
    {
        tb_semaphore_signal(&commit_inside, 1);
        tb_semaphore_wait(&commit_release);
    }
    commit_calls++;
    if (commit_calls == fail_commit)
    {
        return TB_VM_NO_MEMORY;
    }
    return tb_vm_commit(region, offset, size);
}
static tb_vm_result decommit_memory(tb_vm_region *region, size_t offset, size_t size)
{
    if (refuse_decommit)
    {
        return TB_VM_OS_ERROR;
    }
    return tb_vm_decommit(region, offset, size);
}
static tb_vm_result release_memory(tb_vm_region *region)
{
    tb_vm_result result = {0};
    result = tb_vm_release(region);
    if (result == TB_VM_OK)
    {
        live_reservations--;
    }
    return result;
}
static const tb_arena_vm_operations operations = {reserve_memory, commit_memory, decommit_memory,
                                                  release_memory};
TB_TEST(core_reserved_arena_failure, failed_commit_preserves_every_object_mark_and_account)
{
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    refuse_decommit = false;
    refuse_reservation = false;
    for (size_t failure = {0}; failure < 12; failure++)
    {
        tb_arena *arena = NULL;
        unsigned char *objects[12] = {0};
        size_t count = {0};
        commit_calls = 0;
        fail_commit = failure + 1;
        arena = tb_arena_try_create_reserved_with_operations(info.page_size * 32, &operations);
        if (arena == NULL)
        {
            TB_TEST_EXPECT_UINT(failure, ==, 0);
            TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
            continue;
        }
        for (size_t attempt = {0}; attempt < 12; attempt++)
        {
            size_t before = {0};
            size_t committed = {0};
            tb_arena_mark mark = {0};
            unsigned char *memory = NULL;
            before = tb_arena_remaining(arena);
            committed = tb_arena_committed_size(arena);
            mark = tb_arena_get_mark(arena);
            memory = tb_arena_try_alloc(arena, info.page_size, 1);
            if (memory == NULL)
            {
                tb_arena_mark after = {0};
                after = tb_arena_get_mark(arena);
                TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, before);
                TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, committed);
                TB_TEST_EXPECT_POINTER(after.chunk, ==, mark.chunk);
                TB_TEST_EXPECT_POINTER(after.cursor, ==, mark.cursor);
                for (size_t previous = {0}; previous < count; previous++)
                {
                    TB_TEST_EXPECT_UINT(objects[previous][info.page_size - 1], ==, previous + 1);
                }
                fail_commit = 0;
                memory = tb_arena_try_alloc(arena, info.page_size, 1);
                TB_TEST_REQUIRE_POINTER(memory, !=, NULL);
                memset(memory, 42, info.page_size);
                break;
            }
            objects[count] = memory;
            count++;
            memset(memory, (int)count, info.page_size);
        }
        tb_arena_destroy(arena);
        TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
    }
    refuse_reservation = true;
    TB_TEST_EXPECT_POINTER(tb_arena_try_create_reserved_with_operations(4096, &operations), ==,
                           NULL);
    TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
    refuse_reservation = false;
}
TB_TEST(core_reserved_arena_failure, allocator_growth_failure_and_trim_failure_leave_usable_memory)
{
    tb_arena *arena = NULL;
    tb_vm_info info = {0};
    tb_allocator allocator = {0};
    unsigned char *bytes = NULL;
    size_t committed = {0};
    info = tb_vm_get_info();
    commit_calls = 0;
    fail_commit = 0;
    refuse_decommit = false;
    arena = tb_arena_try_create_reserved_with_operations(info.page_size * 32, &operations);
    TB_TEST_REQUIRE_POINTER(arena, !=, NULL);
    allocator = tb_arena_allocator(arena);
    bytes = tb_allocator_alloc(&allocator, 100, 1);
    memset(bytes, 42, 100);
    committed = tb_arena_committed_size(arena);
    fail_commit = commit_calls + 1;
    TB_TEST_EXPECT_POINTER(tb_allocator_try_realloc(&allocator, bytes, 100, info.page_size * 4, 1),
                           ==, NULL);
    TB_TEST_EXPECT_UINT(bytes[99], ==, 42);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, committed);
    fail_commit = 0;
    TB_TEST_EXPECT_POINTER(tb_allocator_try_realloc(&allocator, bytes, 100, info.page_size * 4, 1),
                           ==, bytes);
    committed = tb_arena_committed_size(arena);
    tb_arena_set_commit_retention(arena, 0);
    refuse_decommit = true;
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, committed);
    bytes = tb_arena_alloc(arena, info.page_size * 4, 1);
    memset(bytes, 12, info.page_size * 4);
    refuse_decommit = false;
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, info.page_size);
    tb_arena_destroy(arena);
    TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
}
static void grow_the_arena(void *context)
{
    unsigned char *const bytes = tb_arena_alloc(context, (size_t)256 * 1024, 1);
    bytes[0] = 1;
}
static void allocate_from_the_test(void *context)
{
    (void)tb_arena_alloc(context, 16, 1);
}
/* While one thread is inside an arena, committing the pages it grows into,
 * another that allocates from it fails the check that one thread at a time
 * changes an arena. Once the first has left, the second may. */
TB_TEST(core_reserved_arena_failure, two_threads_in_one_arena_fail_the_check)
{
    tb_arena *arena = NULL;
    tb_thread thread = {0};
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    commit_calls = 0;
    fail_commit = 0;
    refuse_decommit = false;
    arena = tb_arena_try_create_reserved_with_operations((size_t)4 * 1024 * 1024, &operations);
    TB_TEST_REQUIRE_POINTER(arena, !=, NULL);
    tb_platform_atomic_u32_store(&hold_commit, 1);
    tb_thread_create(&thread, grow_the_arena, arena);
    tb_semaphore_wait(&commit_inside);
    TB_TEST_EXPECT_TRAP(allocate_from_the_test, arena, "one thread at a time");
    tb_semaphore_signal(&commit_release, 1);
    tb_thread_join(&thread);
    allocate_from_the_test(arena);
    tb_arena_destroy(arena);
    TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
}
/* Growing one page per commit made the system calls the cost of a
 * reserving arena's allocations, so it commits a step of 64 KiB at a time,
 * and never more than a step past what it hands out. */
TB_TEST(core_reserved_arena_failure, growth_commits_in_steps_and_stays_close_to_use)
{
    enum
    {
        step = 64 * 1024,
        used = 1024 * 1024,
        block = 64
    };
    tb_arena *arena = NULL;
    tb_vm_info info = {0};
    size_t calls = {0};
    info = tb_vm_get_info();
    commit_calls = 0;
    fail_commit = 0;
    refuse_decommit = false;
    arena = tb_arena_try_create_reserved_with_operations((size_t)4 * used, &operations);
    TB_TEST_REQUIRE_POINTER(arena, !=, NULL);
    calls = commit_calls;
    for (size_t offset = {0}; offset < used; offset += block)
    {
        unsigned char *const bytes = tb_arena_alloc(arena, block, 1);
        bytes[0] = 1;
        TB_TEST_EXPECT(tb_arena_committed_size(arena) <=
                       offset + block + step + info.page_size * 2);
    }
    TB_TEST_EXPECT_UINT(commit_calls - calls, <=, used / step + 1);
    tb_arena_destroy(arena);
    TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
}
/* The last step stops at the end of the reservation, which every byte of
 * its capacity can then use. */
TB_TEST(core_reserved_arena_failure, growth_stops_at_the_end_of_the_reservation)
{
    tb_arena *arena = NULL;
    tb_vm_info info = {0};
    unsigned char *bytes = NULL;
    size_t capacity = {0};
    info = tb_vm_get_info();
    commit_calls = 0;
    fail_commit = 0;
    refuse_decommit = false;
    arena = tb_arena_try_create_reserved_with_operations(info.page_size * 3, &operations);
    TB_TEST_REQUIRE_POINTER(arena, !=, NULL);
    capacity = tb_arena_capacity(arena);
    bytes = tb_arena_alloc(arena, 1, 1);
    bytes[0] = 1;
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), <=,
                        tb_arena_reserved_size(arena) - info.page_size);
    bytes = tb_arena_alloc(arena, capacity - 1, 1);
    bytes[capacity - 2] = 2;
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==,
                        tb_arena_reserved_size(arena) - info.page_size);
    tb_arena_destroy(arena);
    TB_TEST_EXPECT_UINT(live_reservations, ==, 0);
}
