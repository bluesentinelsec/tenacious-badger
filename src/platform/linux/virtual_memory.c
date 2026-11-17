#define _DEFAULT_SOURCE
#include "../shared/virtual_memory.h"
#include <stdio.h>
#include <sys/mman.h>
size_t tb_native_vm_large_page_size(void)
{
    FILE *file = NULL;
    char line[256] = {0};
    size_t result = {0};
    file = fopen("/proc/meminfo", "r");
    if (file == NULL)
    {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL)
    {
        unsigned long long kilobytes = {0};
        if (sscanf(line, "Hugepagesize: %llu kB", &kilobytes) == 1 && kilobytes <= SIZE_MAX / 1024)
        {
            result = (size_t)kilobytes * 1024;
            break;
        }
    }
    (void)fclose(file);
    return result;
}
void *tb_native_vm_allocate_large(size_t size)
{
    void *address = NULL;
    address =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    return address == MAP_FAILED ? NULL : address;
}
