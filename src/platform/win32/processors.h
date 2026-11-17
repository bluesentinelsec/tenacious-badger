/* Private to the Windows adapter: the counting of processors, apart from the
 * call that asks the system about them, so that it can be tested with
 * machines nobody has on a desk. */
#ifndef TB_WIN32_PROCESSORS_H
#define TB_WIN32_PROCESSORS_H

#include "badger/system.h"

#include <stdbool.h>
#include <stdint.h>

/* Windows numbers logical processors within groups of at most 64, and names
 * a set of them by a group and a mask of that group's processors. */

/* The processors a process may run on, when the system confines it to some
 * of one group. */
typedef struct tb_win32_affinity
{
    bool is_limited; /* False when every processor of every group is allowed. */
    uint16_t group;  /* The group the process is confined to. */
    uint64_t mask;   /* Its allowed processors in that group. */
} tb_win32_affinity;

/* A count in progress. Zero-initialize it, set `affinity`, add every core of
 * the machine, then read the result with tb_win32_processors_finish(). */
typedef struct tb_win32_processor_tally
{
    tb_win32_affinity affinity;
    size_t logical_count;       /* Allowed logical processors so far. */
    size_t core_count;          /* Cores with at least one of them. */
    uint8_t fastest_class;      /* Highest efficiency class among those cores. */
    size_t fastest_class_count; /* Cores of that class. */
} tb_win32_processor_tally;

/* Adds one core: its efficiency class, where a higher class is a faster
 * core, and the logical processors it presents, as a group and a mask. A
 * core that spans groups is added once for each. `tally` is required
 * non-null. */
void tb_win32_processors_add_core(tb_win32_processor_tally *tally, uint8_t efficiency_class,
                                  uint16_t group, uint64_t mask);

/* Writes the counts. A tally with no allowed processor, which no running
 * process has, gives one of each. Both pointers are required non-null. */
void tb_win32_processors_finish(const tb_win32_processor_tally *tally,
                                tb_system_processors *processors);

#endif
