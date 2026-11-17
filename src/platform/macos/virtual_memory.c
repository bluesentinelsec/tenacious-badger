#define _DARWIN_C_SOURCE
#include "../shared/virtual_memory.h"
#include <mach/vm_statistics.h>
#include <sys/mman.h>
size_t tb_native_vm_large_page_size(void)
{
#if defined(__x86_64__)
    return (size_t)2 * 1024 * 1024;
#else
    /* XNU only exposes explicit 2 MiB superpages on x86_64. ARM's regular
     * pages and transparent promotion remain managed by the operating system. */
    return 0;
#endif
}
void *tb_native_vm_allocate_large(size_t size)
{
#if defined(__x86_64__)
    void *address = NULL;
    address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                   VM_FLAGS_SUPERPAGE_SIZE_2MB, 0);
    return address == MAP_FAILED ? NULL : address;
#else
    (void)size;
    return NULL;
#endif
}
