/** @file core.h
 * @brief Platform-independent core arithmetic.
 */
#ifndef TB_CORE_H
#define TB_CORE_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Add two signed integers after checking for overflow.
 * @param left First operand; any int32_t value is valid.
 * @param right Second operand; any int32_t value is valid.
 * @param[out] result Required non-null caller-owned output, valid for this call.
 * @return true on success; false on overflow, leaving *result unchanged.
 * @pre result points to writable int32_t storage.
 * @par Ownership No allocations or retained pointers.
 * @par Threading Thread-safe for independent outputs; callers synchronize
 * concurrent access to the same output.
 */
TB_API bool tb_sum_i32(int32_t left, int32_t right, int32_t *result);

#endif
