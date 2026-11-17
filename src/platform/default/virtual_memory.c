#include "badger/virtual_memory.h"
#include "not_implemented.h"

tb_vm_info tb_vm_get_info(void)
{
    tb_default_not_implemented("platform.memory", "tb_vm_get_info");
}

tb_vm_result tb_vm_reserve(size_t size, tb_vm_region *region)
{
    (void)size;
    (void)region;
    tb_default_not_implemented("platform.memory", "tb_vm_reserve");
}

tb_vm_result tb_vm_commit(tb_vm_region *region, size_t offset, size_t size)
{
    (void)region;
    (void)offset;
    (void)size;
    tb_default_not_implemented("platform.memory", "tb_vm_commit");
}

tb_vm_result tb_vm_decommit(tb_vm_region *region, size_t offset, size_t size)
{
    (void)region;
    (void)offset;
    (void)size;
    tb_default_not_implemented("platform.memory", "tb_vm_decommit");
}

tb_vm_result tb_vm_release(tb_vm_region *region)
{
    (void)region;
    tb_default_not_implemented("platform.memory", "tb_vm_release");
}

tb_vm_result tb_vm_allocate_large(size_t size, tb_vm_region *region)
{
    (void)size;
    (void)region;
    tb_default_not_implemented("platform.memory", "tb_vm_allocate_large");
}
