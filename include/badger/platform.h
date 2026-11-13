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

/**
 * @brief End the process abnormally, at once.
 *
 * The last step of an unrecoverable failure, such as a failed assertion, after
 * the failure has been reported. An attached debugger stops at the call. With
 * no debugger the process ends with a failure status and never waits on an
 * error dialog, so unattended runs fail fast.
 *
 * @par Errors Does not return.
 * @par Ownership Releases nothing and runs no exit handlers. Flush any output
 * that must survive before calling.
 * @par Threading Thread-safe; the first call ends the process.
 */
TB_API _Noreturn void tb_platform_abort(void);

#endif
