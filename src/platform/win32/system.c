#include "badger/system.h"

#include "badger/assert.h"
#include "badger/platform.h"
#include "processors.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Counting, apart from the system.
 * ------------------------------------------------------------------------- */

static size_t tb_win32_count_bits(uint64_t mask)
{
    size_t count = 0;

    while (mask != 0)
    {
        /* Clears the lowest set bit. */
        mask &= mask - 1;
        count++;
    }
    return count;
}

void tb_win32_processors_add_core(tb_win32_processor_tally *tally, uint8_t efficiency_class,
                                  uint16_t group, uint64_t mask)
{
    uint64_t allowed = mask;
    size_t allowed_count = 0;

    if (tally->affinity.is_limited)
    {
        allowed = 0;
        if (group == tally->affinity.group)
        {
            allowed = mask & tally->affinity.mask;
        }
    }
    allowed_count = tb_win32_count_bits(allowed);
    if (allowed_count == 0)
    {
        return;
    }
    tally->logical_count += allowed_count;
    tally->core_count++;
    if (tally->fastest_class_count == 0 || efficiency_class > tally->fastest_class)
    {
        tally->fastest_class = efficiency_class;
        tally->fastest_class_count = 1;
    }
    else if (efficiency_class == tally->fastest_class)
    {
        tally->fastest_class_count++;
    }
}

void tb_win32_processors_finish(const tb_win32_processor_tally *tally,
                                tb_system_processors *processors)
{
    processors->logical_count = tally->logical_count;
    processors->core_count = tally->core_count;
    processors->performance_core_count = tally->fastest_class_count;
    if (tally->core_count == 0)
    {
        processors->logical_count = 1;
        processors->core_count = 1;
        processors->performance_core_count = 1;
    }
}

/* ---------------------------------------------------------------------------
 * Asking the system.
 * ------------------------------------------------------------------------- */

static _Noreturn void tb_win32_system_fail(const char *diagnostic)
{
    fputs(diagnostic, stderr);
    tb_platform_abort();
}

/* A process is confined to part of the machine when all of its threads are
 * in one processor group, which is every process on a machine of 64 logical
 * processors or fewer. A process that spans groups reports no mask, and may
 * run anywhere in them. */
static void tb_win32_read_affinity(tb_win32_affinity *affinity)
{
    USHORT groups[1] = {0};
    USHORT group_count = 1;
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;

    affinity->is_limited = false;
    if (!GetProcessGroupAffinity(GetCurrentProcess(), &group_count, groups))
    {
        /* More groups than the one there is room for. */
        return;
    }
    if (group_count != 1)
    {
        return;
    }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
    {
        tb_win32_system_fail("platform.system: tb_system_get_processors on win32: "
                             "GetProcessAffinityMask failed\n");
    }
    if (process_mask == 0)
    {
        return;
    }
    affinity->is_limited = true;
    affinity->group = groups[0];
    affinity->mask = (uint64_t)process_mask;
}

/* The system describes each core in a record of its own length. */
static void tb_win32_add_cores(tb_win32_processor_tally *tally, const BYTE *records,
                               DWORD records_size)
{
    DWORD offset = 0;

    while (offset < records_size)
    {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *const record =
            (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(records + offset);

        if (record->Size == 0)
        {
            tb_win32_system_fail("platform.system: tb_system_get_processors on win32: the "
                                 "system described a processor in no bytes\n");
        }
        if (record->Relationship == RelationProcessorCore)
        {
            for (WORD index = 0; index < record->Processor.GroupCount; index++)
            {
                const GROUP_AFFINITY *const set = &record->Processor.GroupMask[index];

                tb_win32_processors_add_core(tally, record->Processor.EfficiencyClass, set->Group,
                                             (uint64_t)set->Mask);
            }
        }
        offset += record->Size;
    }
}

void tb_system_get_processors(tb_system_processors *processors)
{
    tb_win32_processor_tally tally = {0};
    const HANDLE heap = GetProcessHeap();
    BYTE *records = NULL;
    DWORD records_size = 0;

    TB_ASSERT(processors != NULL);
    tb_win32_read_affinity(&tally.affinity);

    /* The system says how many bytes its answer takes, and the answer can
     * grow between the two calls when a processor is added, so ask again
     * until it fits. */
    while (!GetLogicalProcessorInformationEx(
        RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)records, &records_size))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            tb_win32_system_fail("platform.system: tb_system_get_processors on win32: "
                                 "GetLogicalProcessorInformationEx failed\n");
        }
        if (records != NULL)
        {
            (void)HeapFree(heap, 0, records);
        }
        records = HeapAlloc(heap, 0, records_size);
        if (records == NULL)
        {
            tb_win32_system_fail("platform.system: tb_system_get_processors on win32: no "
                                 "memory for the system's description of the processors\n");
        }
    }
    tb_win32_add_cores(&tally, records, records_size);
    if (records != NULL)
    {
        (void)HeapFree(heap, 0, records);
    }
    tb_win32_processors_finish(&tally, processors);
}
