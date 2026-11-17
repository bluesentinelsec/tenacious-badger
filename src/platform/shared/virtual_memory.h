#ifndef TB_NATIVE_VM_H
#define TB_NATIVE_VM_H
#include "badger/assert.h"
#include "badger/virtual_memory.h"
#include <stdint.h>
/* Platform-specific explicit large pages, beneath the shared POSIX operations. */
size_t tb_native_vm_large_page_size(void);
void *tb_native_vm_allocate_large(size_t size);
static inline bool tb_vm_round_size(size_t size, size_t page, size_t *rounded)
{
    size_t remainder = {0};
    if (size == 0 || page == 0 || size > PTRDIFF_MAX)
    {
        return false;
    }
    remainder = size % page;
    if (remainder != 0)
    {
        if (page - remainder > (size_t)PTRDIFF_MAX - size)
        {
            return false;
        }
        size += page - remainder;
    }
    *rounded = size;
    return true;
}
static inline bool tb_vm_valid_range(const tb_vm_region *region, size_t offset, size_t size,
                                     size_t page)
{
    TB_ASSERT(region != NULL && region->address != NULL);
    return !region->large_pages && page != 0 && size != 0 && offset % page == 0 &&
           size % page == 0 && offset <= region->size && size <= region->size - offset;
}
#endif
