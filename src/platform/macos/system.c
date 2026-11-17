/* sysctlbyname is an Apple function; request it from the strict C17 headers. */
#define _DARWIN_C_SOURCE

#include "badger/system.h"
#include "badger/fatal.h"

#include "badger/assert.h"
#include "badger/platform.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include <sys/types.h>

/* Reads a count that the system keeps as a 32-bit integer. Returns false
 * when the system has no value of that name, or the value is not a count. */
static bool tb_macos_read_count(const char *name, size_t *count)
{
    int32_t value = 0;
    size_t size = sizeof(value);

    /* coverage-off: the values asked for below exist on the Macs that run the tests, and are counts
     */
    if (sysctlbyname(name, &value, &size, NULL, 0) != 0)
    {
        return false;
    }
    if (size != sizeof(value) || value < 1)
    {
        return false;
    }
    /* coverage-on */
    *count = (size_t)value;
    return true;
}

void tb_system_get_processors(tb_system_processors *processors)
{
    size_t logical_count = 0;
    size_t core_count = 0;
    size_t performance_core_count = 0;

    TB_ASSERT(processors != NULL);
    /* macOS does not confine a process to some processors, so what the
     * system has available is what the process may use. */
    /* coverage-off: every macOS has this value */
    if (!tb_macos_read_count("hw.logicalcpu", &logical_count))
    {
        tb_fatal_message(
            "platform.system: tb_system_get_processors on macos: sysctlbyname(hw.logicalcpu) "
            "failed\n");
    }
    /* coverage-on */
    /* coverage-off: needs a Mac whose system does not say how many cores it has */
    if (!tb_macos_read_count("hw.physicalcpu", &core_count) || core_count > logical_count)
    {
        core_count = logical_count;
    }
    /* coverage-on */
    /* Performance level 0 is the fastest kind of core. Machines whose cores
     * are all alike, and systems before macOS 12, do not have the value. */
    /* coverage-off: needs a Mac with cores of one kind, or a system from before macOS 12 */
    if (!tb_macos_read_count("hw.perflevel0.physicalcpu", &performance_core_count) ||
        performance_core_count > core_count)
    {
        performance_core_count = core_count;
    }
    /* coverage-on */
    processors->logical_count = logical_count;
    processors->core_count = core_count;
    processors->performance_core_count = performance_core_count;
}

/* macOS says how many processors and cores there are, and of each kind, but
 * not which processor is which. Each is listed as a core of its own, of the
 * fastest kind; threads cannot be confined here, so nothing is lost. */
size_t tb_system_list_processors(tb_system_processor *processors, size_t capacity)
{
    tb_system_processors counts = {0};

    TB_ASSERT(processors != NULL || capacity == 0);
    tb_system_get_processors(&counts);
    for (size_t index = 0; index < counts.logical_count && index < capacity; index++)
    {
        processors[index].core = index;
        processors[index].is_performance = true;
    }
    return counts.logical_count;
}
