/* sched_getaffinity and the macros of its processor set are GNU extensions;
 * request them from the strict C17 headers. */
#define _GNU_SOURCE

#include "badger/system.h"

#include "badger/assert.h"
#include "processors.h"

#include <sched.h>
#include <stdio.h>
#include <unistd.h>

TB_STATIC_ASSERT((size_t)CPU_SETSIZE == TB_LINUX_CPU_LIMIT,
                 "the engine's set of processors is as large as the system's");

/* The kernel's files about processors are a line or two of text. */
static bool tb_linux_read_system_file(void *context, const char *path, char *buffer,
                                      size_t capacity)
{
    FILE *const file = fopen(path, "r");
    size_t length = 0;
    bool is_whole = false;

    (void)context;
    if (file == NULL)
    {
        return false;
    }
    /* One byte is kept for the terminator. A read that stops short of the
     * room it had reached the end of the file. */
    length = fread(buffer, 1, capacity - 1, file);
    is_whole = feof(file) != 0 && ferror(file) == 0;
    (void)fclose(file);
    buffer[length] = '\0';
    return is_whole;
}

/* Used when the system's set of allowed processors cannot be read, which is
 * so on a machine with more processors than the set has room for: the
 * processors that are online, taken to be cores of one kind. */
/* coverage-off: needs a machine with more processors than the set of allowed ones has room for */
static void tb_linux_count_online_processors(tb_system_processors *processors)
{
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    size_t count = 1;

    if (online > 1)
    {
        count = (size_t)online;
    }
    processors->logical_count = count;
    processors->core_count = count;
    processors->performance_core_count = count;
}
/* coverage-on */

void tb_system_get_processors(tb_system_processors *processors)
{
    cpu_set_t native = {0};
    tb_linux_cpu_set allowed = {0};
    size_t allowed_count = 0;

    TB_ASSERT(processors != NULL);
    CPU_ZERO(&native);
    /* Linux confines threads, not processes. The first thread of the process
     * stands for it: the limits that a container or a launcher sets are its
     * limits, and every thread starts with them. */
    /* coverage-off: as above: the set cannot be read only on such a machine */
    if (sched_getaffinity(getpid(), sizeof(native), &native) != 0)
    {
        tb_linux_count_online_processors(processors);
        return;
    }
    /* coverage-on */
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (CPU_ISSET(cpu, &native))
        {
            tb_linux_cpu_set_add(&allowed, cpu);
            allowed_count++;
        }
    }
    /* coverage-off: a running thread is allowed on at least one processor */
    if (allowed_count == 0)
    {
        tb_linux_count_online_processors(processors);
        return;
    }
    /* coverage-on */
    tb_linux_count_processors(&allowed, tb_linux_read_system_file, NULL, processors);
}
