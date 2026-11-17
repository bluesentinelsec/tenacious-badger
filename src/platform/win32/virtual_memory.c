#define WIN32_LEAN_AND_MEAN
#include "../shared/virtual_memory.h"
#include <windows.h>

tb_vm_info tb_vm_get_info(void)
{
    SYSTEM_INFO native = {0};
    tb_vm_info info = {0};
    GetSystemInfo(&native);
    info.page_size = native.dwPageSize;
    info.allocation_granularity = native.dwAllocationGranularity;
    info.large_page_size = GetLargePageMinimum();
    return info;
}
tb_vm_result tb_vm_reserve(size_t size, tb_vm_region *region)
{
    tb_vm_info info = {0};
    void *address = NULL;
    size_t rounded = {0};
    TB_ASSERT(region != NULL && region->address == NULL && region->size == 0);
    info = tb_vm_get_info();
    if (!tb_vm_round_size(size, info.page_size, &rounded))
    {
        return TB_VM_INVALID;
    }
    address = VirtualAlloc(NULL, rounded, MEM_RESERVE, PAGE_NOACCESS);
    if (address == NULL)
    {
        return TB_VM_NO_MEMORY;
    }
    region->address = address;
    region->size = rounded;
    return TB_VM_OK;
}
tb_vm_result tb_vm_commit(tb_vm_region *region, size_t offset, size_t size)
{
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    if (!tb_vm_valid_range(region, offset, size, info.page_size))
    {
        return TB_VM_INVALID;
    }
    return VirtualAlloc((unsigned char *)region->address + offset, size, MEM_COMMIT,
                        PAGE_READWRITE) != NULL
               ? TB_VM_OK
               : TB_VM_NO_MEMORY;
}
tb_vm_result tb_vm_decommit(tb_vm_region *region, size_t offset, size_t size)
{
    tb_vm_info info = {0};
    info = tb_vm_get_info();
    if (!tb_vm_valid_range(region, offset, size, info.page_size))
    {
        return TB_VM_INVALID;
    }
    /* The analyzer warns that pages freed without MEM_RELEASE keep their
     * address range. Keeping it is what decommitting is: the range stays
     * reserved for the region until tb_vm_release() gives it back. */
#pragma warning(suppress : 6250)
    return VirtualFree((unsigned char *)region->address + offset, size, MEM_DECOMMIT)
               ? TB_VM_OK
               : TB_VM_OS_ERROR;
}
tb_vm_result tb_vm_release(tb_vm_region *region)
{
    TB_ASSERT(region != NULL);
    if (region->address == NULL)
    {
        return TB_VM_OK;
    }
    if (!VirtualFree(region->address, 0, MEM_RELEASE))
    {
        return TB_VM_OS_ERROR;
    }
    *region = (tb_vm_region){0};
    return TB_VM_OK;
}
tb_vm_result tb_vm_allocate_large(size_t size, tb_vm_region *region)
{
    tb_vm_info info = {0};
    void *address = NULL;
    TB_ASSERT(region != NULL && region->address == NULL && region->size == 0);
    info = tb_vm_get_info();
    if (info.large_page_size == 0)
    {
        return TB_VM_UNAVAILABLE;
    }
    if (size == 0 || size > PTRDIFF_MAX || size % info.large_page_size != 0)
    {
        return TB_VM_INVALID;
    }
    address = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (address == NULL)
    {
        return TB_VM_UNAVAILABLE;
    }
    region->address = address;
    region->size = size;
    region->large_pages = true;
    return TB_VM_OK;
}
