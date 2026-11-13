#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void *tb_test_allocate_zeroed(size_t count, size_t element_size)
{
    void *memory = NULL;

    /* calloc(0, n) may return NULL; one element keeps "NULL means failure". */
    if (count == 0)
    {
        count = 1;
    }
    memory = calloc(count, element_size);
    if (memory == NULL)
    {
        tb_test_misuse("out of memory allocating %zu objects of %zu bytes", count, element_size);
    }
    return memory;
}

void tb_test_misuse(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)fflush(stdout);
    fputs("tb_test: ", stderr);
    (void)vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    abort();
}
