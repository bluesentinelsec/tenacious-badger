/** @file system.h
 * @brief What the machine offers the process: its processors. Implementations
 * are build-selected.
 */
#ifndef TB_SYSTEM_H
#define TB_SYSTEM_H

#include "badger/api.h"

#include <stddef.h>

/**
 * @brief The processors that the process may run on.
 *
 * The counts describe what the process may use, not what the machine has: a
 * process that the system restricts to some processors counts only those.
 *
 * A *logical processor* is what the system schedules a thread on. A *core*
 * is the hardware that runs it, and may present two or more logical
 * processors that share it. Some machines have cores of several kinds, fast
 * ones and slower, more efficient ones; the *performance cores* are those of
 * the fastest kind.
 *
 * @code
 * tb_system_processors processors = {0};
 *
 * tb_system_get_processors(&processors);
 * // Windows laptop, 8 fast cores of two threads and 16 efficient cores:
 * //   logical_count 32, core_count 24, performance_core_count 8
 * // Steam Deck, 4 cores of two threads:
 * //   logical_count 8, core_count 4, performance_core_count 4
 * @endcode
 */
typedef struct tb_system_processors
{
    /** Logical processors the process may run on; at least 1. */
    size_t logical_count;
    /** Cores that have at least one of those logical processors; from 1 to
     * `logical_count`. Equal to `logical_count` where the system does not say
     * which logical processors share a core. */
    size_t core_count;
    /** Cores of the fastest kind among them; from 1 to `core_count`. Equal to
     * `core_count` where every core is of one kind, and where the system does
     * not say. `core_count` less this is the number of slower cores. */
    size_t performance_core_count;
} tb_system_processors;

/**
 * @brief Count the processors that the process may run on.
 *
 * The system is asked on every call, because the answer can change while the
 * process runs: a processor can be taken offline, and the process can be
 * restricted to fewer. A call costs system calls, and on Linux reads of
 * several small files, so read it once where it is needed and keep the answer.
 *
 * A limit on processor time, such as the quota a container may be given, is
 * not a limit on processors and is not counted.
 *
 * @param[out] processors Required non-null; receives the counts.
 * @par Errors A system that cannot be asked ends the process with a
 * diagnostic.
 * @par Ownership No allocations are left behind and nothing is kept.
 * @par Threading Thread-safe.
 */
TB_API void tb_system_get_processors(tb_system_processors *processors);

#endif
