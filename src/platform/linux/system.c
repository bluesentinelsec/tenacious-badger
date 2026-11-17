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

/* The processors that the process may use, as the system's set of them.
 * False when the set cannot be read, which is so on a machine with more
 * processors than the set has room for. Linux confines threads, not
 * processes: the first thread of the process stands for it, since the
 * limits that a container or a launcher sets are its limits, and every
 * thread starts with them. */
bool tb_linux_read_process_processors(tb_linux_cpu_set *allowed)
{
    cpu_set_t native;
    bool has_any = false;

    CPU_ZERO(&native);
    /* coverage-off: needs a machine with more processors than the set of allowed ones has room for
     */
    if (sched_getaffinity(getpid(), sizeof(native), &native) != 0)
    {
        return false;
    }
    /* coverage-on */
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (CPU_ISSET(cpu, &native))
        {
            tb_linux_cpu_set_add(allowed, cpu);
            has_any = true;
        }
    }
    return has_any;
}

/* When the set cannot be read: the processors that are online, taken to be
 * cores of one kind, each a core by itself. */
/* coverage-off: as above */
static size_t tb_linux_list_online_processors(tb_system_processor *list, size_t capacity)
{
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    size_t count = 1;

    if (online > 1)
    {
        count = (size_t)online;
    }
    for (size_t index = 0; index < count && index < capacity; index++)
    {
        list[index].core = index;
        list[index].is_performance = true;
    }
    return count;
}
/* coverage-on */

void tb_system_get_processors(tb_system_processors *processors)
{
    tb_linux_cpu_set allowed = {0};

    TB_ASSERT(processors != NULL);
    if (!tb_linux_read_process_processors(&allowed))
    {
        /* coverage-off: as above */
        const size_t count = tb_linux_list_online_processors(NULL, 0);

        processors->logical_count = count;
        processors->core_count = count;
        processors->performance_core_count = count;
        return;
        /* coverage-on */
    }
    tb_linux_count_processors(&allowed, tb_linux_read_system_file, NULL, processors);
}

size_t tb_system_list_processors(tb_system_processor *processors, size_t capacity)
{
    tb_linux_cpu_set allowed = {0};

    TB_ASSERT(processors != NULL || capacity == 0);
    if (!tb_linux_read_process_processors(&allowed))
    {
        return tb_linux_list_online_processors(processors, capacity);
    }
    return tb_linux_list_processors(&allowed, tb_linux_read_system_file, NULL, processors, capacity,
                                    NULL);
}
