#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "../shared/virtual_memory.h"
#include <sys/mman.h>
#include <unistd.h>

tb_vm_info tb_vm_get_info(void)
{
    tb_vm_info info = {0};
    long page = {0};
    page = sysconf(_SC_PAGESIZE);
    if (page > 0)
    {
        info.page_size = (size_t)page;
        info.allocation_granularity = (size_t)page;
    }
    info.large_page_size = tb_native_vm_large_page_size();
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
    address = mmap(NULL, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
    {
        return TB_VM_NO_MEMORY;
    }
    region->address = address;
    region->size = rounded;
    return TB_VM_OK;
}
tb_vm_result tb_vm_commit(tb_vm_region *region, size_t offset, size_t size)
{
    long page = {0};
    page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !tb_vm_valid_range(region, offset, size, (size_t)page))
    {
        return TB_VM_INVALID;
    }
    return mprotect((unsigned char *)region->address + offset, size, PROT_READ | PROT_WRITE) == 0
               ? TB_VM_OK
               : TB_VM_NO_MEMORY;
}
tb_vm_result tb_vm_decommit(tb_vm_region *region, size_t offset, size_t size)
{
    long page = {0};
    void *address = NULL;
    page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !tb_vm_valid_range(region, offset, size, (size_t)page))
    {
        return TB_VM_INVALID;
    }
    address = (unsigned char *)region->address + offset;
    /* Replace only pages inside our owned reservation. An anonymous mapping
     * discards physical contents and guarantees zeros on recommit on both
     * POSIX hosts; Darwin's advisory discard alone does not guarantee zeros. */
    return mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
                   MAP_FAILED
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
    if (munmap(region->address, region->size) != 0)
    {
        return TB_VM_OS_ERROR;
    }
    *region = (tb_vm_region){0};
    return TB_VM_OK;
}
tb_vm_result tb_vm_allocate_large(size_t size, tb_vm_region *region)
{
    size_t page = {0};
    void *address = NULL;
    TB_ASSERT(region != NULL && region->address == NULL && region->size == 0);
    page = tb_native_vm_large_page_size();
    if (page == 0)
    {
        return TB_VM_UNAVAILABLE;
    }
    if (size == 0 || size > PTRDIFF_MAX || size % page != 0)
    {
        return TB_VM_INVALID;
    }
    address = tb_native_vm_allocate_large(size);
    if (address == NULL)
    {
        return TB_VM_UNAVAILABLE;
    }
    region->address = address;
    region->size = size;
    region->large_pages = true;
    return TB_VM_OK;
}
