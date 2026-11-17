/* pthread_setname_np, pthread_getname_np, and the quality-of-service classes
 * are Apple extensions; request them from the strict C17 headers. */
#define _DARWIN_C_SOURCE

#include "../posix/thread_native.h"

#include "../shared/name.h"

#include <stdint.h>

#include <pthread.h>
#include <pthread/qos.h>

/* The system keeps 63 bytes of a thread's name and a terminator. */
#define TB_MACOS_THREAD_NAME_CAPACITY ((size_t)64)

void tb_posix_thread_apply_name(const char *name)
{
    /* macOS names only the calling thread. */
    (void)pthread_setname_np(name);
}

void tb_posix_thread_apply_priority(tb_thread_priority priority)
{
    /* macOS schedules by quality-of-service class, which also decides, on a
     * machine with two kinds of core, which kind a thread runs on. */
    qos_class_t quality = QOS_CLASS_DEFAULT;

    if (priority == TB_THREAD_PRIORITY_BACKGROUND)
    {
        quality = QOS_CLASS_UTILITY;
    }
    else if (priority == TB_THREAD_PRIORITY_HIGH)
    {
        quality = QOS_CLASS_USER_INTERACTIVE;
    }
    (void)pthread_set_qos_class_self_np(quality, 0);
}

size_t tb_posix_thread_read_name(char *buffer, size_t capacity)
{
    char kept[TB_MACOS_THREAD_NAME_CAPACITY] = {0};

    /* coverage-off: reading the calling thread's own name into room enough does not fail */
    if (pthread_getname_np(pthread_self(), kept, sizeof(kept)) != 0)
    {
        kept[0] = '\0';
    }
    /* coverage-on */
    return tb_platform_copy_name(buffer, capacity, kept);
}

/* macOS decides by itself where a thread runs. It takes hints of affinity on
 * Intel Macs and none on Apple silicon, and says on neither where a thread
 * is running. */
void tb_posix_thread_confine(pthread_attr_t *attributes, const size_t *processors, size_t count)
{
    (void)attributes;
    (void)processors;
    (void)count;
}

bool tb_posix_thread_can_be_confined(void)
{
    return false;
}

size_t tb_posix_thread_current_processor(void)
{
    return SIZE_MAX;
}
