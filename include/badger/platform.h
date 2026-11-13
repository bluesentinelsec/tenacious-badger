/** @file platform.h
 * @brief Native platform service interfaces. Implementations are build-selected.
 */
#ifndef TB_PLATFORM_H
#define TB_PLATFORM_H

#include "badger/api.h"

#include <stdint.h>

/**
 * @brief Read the native monotonic performance counter in opaque ticks.
 * @return Counter value on Windows. The epoch and tick period are unspecified;
 * use values only to compare readings during the current process lifetime.
 * @par Errors Native counter failure terminates with a diagnostic. Linux and
 * macOS terminate with a fatal-stub diagnostic naming the planned native API.
 * @par Ownership No allocations or caller-owned resources.
 * @par Threading Thread-safe; no shared mutable engine state.
 */
TB_API uint64_t tb_platform_ticks(void);

#endif
