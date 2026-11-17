#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/virtual_memory.h"
#include "tb_test.h"
#include <stdint.h>
#include <string.h>

TB_TEST(core_reserved_arena, reservation_geometry_and_heap_accounting_are_separate)
{
    static const size_t sizes[] = {0, 1, 255, 4095, 65537};
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        tb_arena *arena = NULL;
        size_t before = {0};
        size_t requested = {0};
        before = tb_memory_allocation_count();
        arena = tb_arena_create_reserved(sizes[index]);
        requested = sizes[index] == 0 ? TB_ARENA_DEFAULT_RESERVE_SIZE : sizes[index];
        TB_TEST_EXPECT(tb_arena_reserved_size(arena) >= requested + info.page_size);
        TB_TEST_EXPECT_UINT(tb_arena_reserved_size(arena) % info.page_size, ==, 0);
        TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, info.page_size);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, tb_arena_capacity(arena));
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, before);
        tb_arena_destroy(arena);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, before);
    }
    TB_TEST_EXPECT_POINTER(tb_arena_try_create_reserved(SIZE_MAX), ==, NULL);
    {
        tb_arena *arena = NULL;
        arena = tb_arena_create(0);
        TB_TEST_EXPECT_UINT(tb_arena_reserved_size(arena), ==, 0);
        TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, 0);
        tb_arena_destroy(arena);
    }
}
TB_TEST(core_reserved_arena, objects_keep_addresses_and_contents_across_many_commits)
{
    tb_arena *arena = NULL;
    unsigned char *objects[256] = {0};
    size_t lengths[256] = {0};
    size_t committed = {0};
    arena = tb_arena_create_reserved((size_t)8 * 1024 * 1024);
    for (size_t object = {0}; object < TB_TEST_COUNT_OF(objects); object++)
    {
        size_t alignment = {0};
        alignment = (size_t)1 << tb_test_random_below(13);
        lengths[object] = 1 + (size_t)tb_test_random_below(8192);
        objects[object] = tb_arena_alloc(arena, lengths[object], alignment);
        TB_TEST_EXPECT_UINT((uintptr_t)objects[object] % alignment, ==, 0);
        memset(objects[object], (int)(object & 255), lengths[object]);
        TB_TEST_EXPECT(tb_arena_committed_size(arena) >= committed);
        committed = tb_arena_committed_size(arena);
        for (size_t previous = {0}; previous <= object; previous++)
        {
            TB_TEST_EXPECT_UINT(objects[previous][0], ==, previous & 255);
            TB_TEST_EXPECT_UINT(objects[previous][lengths[previous] - 1], ==, previous & 255);
        }
    }
    tb_arena_destroy(arena);
}
TB_TEST(core_reserved_arena, reset_trims_to_policy_and_rewind_keeps_commitment)
{
    tb_arena *arena = NULL;
    tb_vm_info info = {0};
    tb_arena_mark mark = {0};
    unsigned char *first = NULL;
    size_t committed = {0};
    info = tb_vm_get_info();
    arena = tb_arena_create_reserved((size_t)2 * 1024 * 1024);
    first = tb_arena_alloc(arena, 64, 1);
    first[0] = 42;
    mark = tb_arena_get_mark(arena);
    (void)tb_arena_alloc(arena, (size_t)512 * 1024, 1);
    committed = tb_arena_committed_size(arena);
    TB_TEST_EXPECT(committed > TB_ARENA_DEFAULT_COMMIT_RETENTION);
    tb_arena_rewind(arena, mark);
    TB_TEST_EXPECT_UINT(first[0], ==, 42);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, committed);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, TB_ARENA_DEFAULT_COMMIT_RETENTION);
    tb_arena_set_commit_retention(arena, 0);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, info.page_size);
    TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, 64, 1), ==, first);
    tb_arena_set_commit_retention(arena, info.page_size + 1);
    (void)tb_arena_alloc(arena, info.page_size * 4, 1);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, info.page_size * 2);
    tb_arena_set_commit_retention(arena, SIZE_MAX);
    (void)tb_arena_alloc(arena, info.page_size * 5, 1);
    committed = tb_arena_committed_size(arena);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_committed_size(arena), ==, committed);
    tb_arena_destroy(arena);
}
static void allocate_after_exhaustion(void *context)
{
    (void)tb_arena_alloc(context, 1, 1);
}
static void create_unrepresentable(void *context)
{
    (void)context;
    (void)tb_arena_create_reserved(SIZE_MAX);
}
TB_TEST(core_reserved_arena, exhaustion_is_bounded_and_fail_fast_has_a_fallible_form)
{
    tb_arena *arena = NULL;
    size_t capacity = {0};
    unsigned char *bytes = NULL;
    arena = tb_arena_create_reserved(0);
    capacity = tb_arena_capacity(arena);
    bytes = tb_arena_alloc(arena, capacity, 1);
    bytes[0] = 1;
    bytes[capacity - 1] = 2;
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
    TB_TEST_EXPECT_POINTER(tb_arena_try_alloc(arena, 1, 1), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_arena_try_calloc(arena, SIZE_MAX, 2, 1), ==, NULL);
    TB_TEST_EXPECT_TRAP(allocate_after_exhaustion, arena, "out of memory");
    TB_TEST_EXPECT_TRAP(create_unrepresentable, NULL, "out of memory");
    TB_TEST_EXPECT_UINT(bytes[0], ==, 1);
    TB_TEST_EXPECT_UINT(bytes[capacity - 1], ==, 2);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_POINTER(tb_arena_try_alloc(arena, 1, 1), ==, bytes);
    tb_arena_destroy(arena);
}
TB_TEST(core_reserved_arena, allocator_extends_latest_block_across_commit_boundaries)
{
    tb_arena *arena = NULL;
    tb_allocator allocator = {0};
    tb_vm_info info = {0};
    unsigned char *bytes = NULL;
    unsigned char *grown = NULL;
    unsigned char *moved = NULL;
    info = tb_vm_get_info();
    arena = tb_arena_create_reserved(info.page_size * 32);
    allocator = tb_arena_allocator(arena);
    bytes = tb_allocator_alloc(&allocator, 100, 16);
    memset(bytes, 42, 100);
    grown = tb_allocator_realloc(&allocator, bytes, 100, info.page_size * 3, 16);
    TB_TEST_EXPECT_POINTER(grown, ==, bytes);
    TB_TEST_EXPECT_UINT(grown[99], ==, 42);
    memset(grown + 100, 24, info.page_size * 3 - 100);
    (void)tb_arena_alloc(arena, 32, 16);
    moved = tb_allocator_realloc(&allocator, grown, info.page_size * 3, info.page_size * 4, 16);
    TB_TEST_EXPECT_POINTER(moved, !=, grown);
    TB_TEST_EXPECT_UINT(moved[99], ==, 42);
    TB_TEST_EXPECT_UINT(moved[info.page_size * 3 - 1], ==, 24);
    tb_allocator_free(&allocator, moved, info.page_size * 4, 16);
    tb_arena_destroy(arena);
}
static void invalid_reserved_contract(void *context)
{
    size_t choice = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        tb_arena_set_commit_retention(NULL, 0);
        break;
    case 1:
        (void)tb_arena_committed_size(NULL);
        break;
    case 2:
        (void)tb_arena_reserved_size(NULL);
        break;
    default:
        break;
    }
}
static void set_retention_on_fixed(void *context)
{
    tb_arena_set_commit_retention(context, 0);
}
TB_TEST(core_reserved_arena, configuration_requires_a_reserved_arena)
{
    static unsigned char buffer[1024];
    tb_arena *fixed = NULL;
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 3; choice++)
    {
        TB_TEST_EXPECT_TRAP(invalid_reserved_contract, &choice, "assertion failed");
    }
    fixed = tb_arena_create_fixed(buffer, sizeof(buffer));
    TB_TEST_EXPECT_TRAP(set_retention_on_fixed, fixed, "assertion failed");
    tb_arena_destroy(fixed);
}
