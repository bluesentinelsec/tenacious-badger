#include "badger/assert.h"
#include "badger/virtual_memory.h"
#include "tb_test.h"
#include <stdint.h>
#include <string.h>

TB_TEST(platform_memory, reserve_commit_discard_recommit_and_release)
{
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    TB_TEST_REQUIRE(info.page_size > 0);
    TB_TEST_REQUIRE(info.allocation_granularity >= info.page_size);
    for (size_t pages = {0}; pages < 24; pages++)
    {
        tb_vm_region region = {0};
        unsigned char *bytes = NULL;
        size_t size = {0};
        size = (pages + 1) * info.page_size;
        TB_TEST_REQUIRE_UINT(tb_vm_reserve(size - 1, &region), ==, TB_VM_OK);
        TB_TEST_EXPECT_UINT(region.size, ==, size);
        TB_TEST_EXPECT_UINT((uintptr_t)region.address % info.allocation_granularity, ==, 0);
        TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, size), ==, TB_VM_OK);
        bytes = region.address;
        for (size_t index = {0}; index < size; index++)
        {
            TB_TEST_REQUIRE_UINT(bytes[index], ==, 0);
        }
        memset(bytes, 0x53, size);
        TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, size), ==, TB_VM_OK);
        TB_TEST_EXPECT_UINT(bytes[size - 1], ==, 0x53);
        TB_TEST_REQUIRE_UINT(tb_vm_decommit(&region, 0, info.page_size), ==, TB_VM_OK);
        TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, info.page_size), ==, TB_VM_OK);
        for (size_t index = {0}; index < info.page_size; index++)
        {
            TB_TEST_REQUIRE_UINT(bytes[index], ==, 0);
        }
        if (pages != 0)
        {
            TB_TEST_EXPECT_UINT(bytes[size - 1], ==, 0x53);
        }
        TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
        TB_TEST_EXPECT_POINTER(region.address, ==, NULL);
        TB_TEST_EXPECT_UINT(region.size, ==, 0);
        TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
    }
}
TB_TEST(platform_memory, large_reservations_commit_only_the_touched_pages)
{
    tb_vm_region region = {0};
    tb_vm_info info = {0};
    size_t size = {0};
    unsigned char *bytes = NULL;
    info = tb_vm_get_info();
    size = (size_t)8 * 1024 * 1024 * 1024;
    TB_TEST_REQUIRE_UINT(tb_vm_reserve(size, &region), ==, TB_VM_OK);
    TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, info.page_size), ==, TB_VM_OK);
    TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, size - info.page_size, info.page_size), ==,
                         TB_VM_OK);
    bytes = region.address;
    bytes[0] = 1;
    bytes[size - 1] = 2;
    TB_TEST_EXPECT_UINT(bytes[0], ==, 1);
    TB_TEST_EXPECT_UINT(bytes[size - 1], ==, 2);
    TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
}
TB_TEST(platform_memory, invalid_ranges_do_not_touch_committed_bytes)
{
    tb_vm_region region = {0};
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    TB_TEST_EXPECT_UINT(tb_vm_reserve(0, &region), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_reserve(SIZE_MAX, &region), ==, TB_VM_INVALID);
    TB_TEST_REQUIRE_UINT(tb_vm_reserve(info.page_size, &region), ==, TB_VM_OK);
    TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, info.page_size), ==, TB_VM_OK);
    *(unsigned char *)region.address = 42;
    TB_TEST_EXPECT_UINT(tb_vm_commit(&region, 1, info.page_size), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_commit(&region, 0, 1), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_commit(&region, 0, 0), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_commit(&region, info.page_size, info.page_size), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_decommit(&region, SIZE_MAX, info.page_size), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(tb_vm_decommit(&region, 0, SIZE_MAX), ==, TB_VM_INVALID);
    TB_TEST_EXPECT_UINT(*(unsigned char *)region.address, ==, 42);
    TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
}
TB_TEST(platform_memory, optional_large_pages_have_a_regular_fallback)
{
    tb_vm_region region = {0};
    tb_vm_info info = {0};
    tb_vm_result result = {0};
    info = tb_vm_get_info();
    result = tb_vm_allocate_large(info.large_page_size, &region);
    TB_TEST_REQUIRE(result == TB_VM_OK || result == TB_VM_UNAVAILABLE);
    if (result == TB_VM_OK)
    {
        TB_TEST_EXPECT(region.large_pages);
        memset(region.address, 0x65, region.size);
        TB_TEST_EXPECT_UINT(tb_vm_commit(&region, 0, info.page_size), ==, TB_VM_INVALID);
        TB_TEST_EXPECT_UINT(tb_vm_decommit(&region, 0, info.page_size), ==, TB_VM_INVALID);
        TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
    }
    else
    {
        TB_TEST_EXPECT_POINTER(region.address, ==, NULL);
        TB_TEST_REQUIRE_UINT(tb_vm_reserve(info.page_size, &region), ==, TB_VM_OK);
        TB_TEST_EXPECT_UINT(tb_vm_release(&region), ==, TB_VM_OK);
    }
    if (info.large_page_size != 0)
    {
        TB_TEST_EXPECT_UINT(tb_vm_allocate_large(0, &region), ==, TB_VM_INVALID);
        TB_TEST_EXPECT_UINT(tb_vm_allocate_large(info.large_page_size - 1, &region), ==,
                            TB_VM_INVALID);
        TB_TEST_EXPECT_UINT(tb_vm_allocate_large(SIZE_MAX, &region), ==, TB_VM_INVALID);
    }
}
static void invalid_vm_contract(void *context)
{
    size_t choice = {0};
    tb_vm_region region = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        (void)tb_vm_reserve(1, NULL);
        break;
    case 1:
        region.size = 1;
        (void)tb_vm_reserve(1, &region);
        break;
    case 2:
        (void)tb_vm_commit(&region, 0, 1);
        break;
    case 3:
        (void)tb_vm_decommit(NULL, 0, 1);
        break;
    case 4:
        (void)tb_vm_release(NULL);
        break;
    case 5:
        (void)tb_vm_allocate_large(1, NULL);
        break;
    default:
        break;
    }
}
TB_TEST(platform_memory, pointer_and_ownership_contracts_are_asserted)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 6; choice++)
    {
        TB_TEST_EXPECT_TRAP(invalid_vm_contract, &choice, "assertion failed");
    }
}
