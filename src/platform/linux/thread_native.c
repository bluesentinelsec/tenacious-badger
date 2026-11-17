/* pthread_setname_np, pthread_getname_np, and the system call that names the
 * calling thread to setpriority are GNU and Linux extensions; request them
 * from the strict C17 headers. */
#define _GNU_SOURCE

#include "../posix/thread_native.h"

#include "../shared/name.h"
#include "processors.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The kernel keeps 15 bytes of a thread's name and a terminator. */
#define TB_LINUX_THREAD_NAME_CAPACITY ((size_t)16)

/* Niceness is the priority of an ordinary Linux thread: higher is lower
 * priority, from -20 to 19, and 0 is normal. */
#define TB_LINUX_NICE_BACKGROUND 10
#define TB_LINUX_NICE_HIGH (-5)

void tb_posix_thread_apply_name(const char *name)
{
    char kept[TB_LINUX_THREAD_NAME_CAPACITY] = {0};

    /* A longer name is refused whole, so it is cut here, between characters. */
    (void)tb_platform_copy_name(kept, sizeof(kept), name);
    (void)pthread_setname_np(pthread_self(), kept);
}

void tb_posix_thread_apply_priority(tb_thread_priority priority)
{
    /* On Linux a niceness belongs to a thread, and a thread is named to the
     * system by its own identifier, which only a system call gives. */
    const id_t thread = (id_t)syscall(SYS_gettid);
    int niceness = 0;

    if (priority == TB_THREAD_PRIORITY_BACKGROUND)
    {
        niceness = TB_LINUX_NICE_BACKGROUND;
    }
    else if (priority == TB_THREAD_PRIORITY_HIGH)
    {
        niceness = TB_LINUX_NICE_HIGH;
    }
    /* Raising a priority needs a privilege that most processes lack. The
     * thread then stays at the normal one. */
    (void)setpriority(PRIO_PROCESS, thread, niceness);
}

size_t tb_posix_thread_read_name(char *buffer, size_t capacity)
{
    char kept[TB_LINUX_THREAD_NAME_CAPACITY] = {0};

    if (pthread_getname_np(pthread_self(), kept, sizeof(kept)) != 0)
    {
        kept[0] = '\0';
    }
    return tb_platform_copy_name(buffer, capacity, kept);
}

/* ---------------------------------------------------------------------------
 * Processors, by their places in the list of the processors that the process
 * may use.
 * ------------------------------------------------------------------------- */

void tb_posix_thread_confine(pthread_attr_t *attributes, const size_t *processors, size_t count)
{
    static _Thread_local tb_linux_processor list[TB_LINUX_CPU_LIMIT];
    const size_t listed = tb_linux_list_process_processors(list, TB_LINUX_CPU_LIMIT);
    cpu_set_t native;
    bool has_any = false;

    CPU_ZERO(&native);
    for (size_t index = 0; index < count; index++)
    {
        if (processors[index] < listed && processors[index] < TB_LINUX_CPU_LIMIT)
        {
            CPU_SET(list[processors[index]].cpu, &native);
            has_any = true;
        }
    }
    /* The system refuses a set with no processor the process may use; the
     * thread then runs where it would have anyway. */
    if (has_any)
    {
        (void)pthread_attr_setaffinity_np(attributes, sizeof(native), &native);
    }
}

bool tb_posix_thread_can_be_confined(void)
{
    return true;
}

size_t tb_posix_thread_current_processor(void)
{
    static _Thread_local tb_linux_processor list[TB_LINUX_CPU_LIMIT];
    const int cpu = sched_getcpu();
    size_t listed = 0;

    /* coverage-off: sched_getcpu fails only on a kernel older than any the engine supports */
    if (cpu < 0)
    {
        return SIZE_MAX;
    }
    /* coverage-on */
    listed = tb_linux_list_process_processors(list, TB_LINUX_CPU_LIMIT);
    for (size_t index = 0; index < listed && index < TB_LINUX_CPU_LIMIT; index++)
    {
        if (list[index].cpu == (uint32_t)cpu)
        {
            return index;
        }
    }
    return SIZE_MAX;
}
