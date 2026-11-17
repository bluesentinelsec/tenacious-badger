#include "badger/fmt.h"
#include "badger/virtual_memory.h"
#include "tb_test.h"

TB_TEST(platform_memory_fault, reserved_page)
{
    tb_vm_region region = {0};
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    TB_TEST_REQUIRE_UINT(tb_vm_reserve(info.page_size, &region), ==, TB_VM_OK);
    tb_fmt_print_error("touching inaccessible page\n");
    TB_TEST_REQUIRE(tb_fmt_flush());
    /* Volatile forces the intentional fault; it is not synchronization. */
    *(volatile unsigned char *)region.address = 1;
}
TB_TEST(platform_memory_fault, decommitted_page)
{
    tb_vm_region region = {0};
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    TB_TEST_REQUIRE_UINT(tb_vm_reserve(info.page_size, &region), ==, TB_VM_OK);
    TB_TEST_REQUIRE_UINT(tb_vm_commit(&region, 0, info.page_size), ==, TB_VM_OK);
    *(unsigned char *)region.address = 42;
    TB_TEST_REQUIRE_UINT(tb_vm_decommit(&region, 0, info.page_size), ==, TB_VM_OK);
    tb_fmt_print_error("touching inaccessible page\n");
    TB_TEST_REQUIRE(tb_fmt_flush());
    *(volatile unsigned char *)region.address = 1;
}
