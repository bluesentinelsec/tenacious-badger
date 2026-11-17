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

/* One logical processor that the process may use: where the system has it,
 * the efficiency class of its core, where a higher class is a faster core,
 * and the number of its core among the allowed cores, counted from zero. */
typedef struct tb_win32_processor
{
    uint16_t group;
    uint8_t number; /* Its place in the group, below 64. */
    uint8_t efficiency_class;
    uint32_t core;
} tb_win32_processor;

/* The most logical processors that a list has room for: 64 groups of 64. */
#define TB_WIN32_PROCESSORS_MAX ((size_t)4096)

/* A count in progress. Zero-initialize it, set `affinity`, and `list` and
 * `list_capacity` to have the processors listed as well as counted; add
 * every core of the machine, then read the result with
 * tb_win32_processors_finish(). */
typedef struct tb_win32_processor_tally
{
    tb_win32_affinity affinity;
    tb_win32_processor *list; /* NULL to count only. */
    size_t list_capacity;
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

/* Asks the system for the processors that the process may use, and lists the
 * first `capacity` of them in the order of tb_system_list_processors().
 * Returns how many there are, and the efficiency class of the fastest core
 * among them in `*fastest_class`. Defined in system.c; the thread functions
 * confine a thread by it. Both pointers are required non-null. */
size_t tb_win32_list_processors(tb_win32_processor *list, size_t capacity, uint8_t *fastest_class);

#endif
