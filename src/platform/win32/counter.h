/* Private to the Windows adapter: the arithmetic that turns a reading of the
 * performance counter into nanoseconds, apart from the reading itself so that
 * it can be tested with counts and frequencies no machine will produce. */
#ifndef TB_WIN32_COUNTER_H
#define TB_WIN32_COUNTER_H

#include <stdint.h>

#define TB_WIN32_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* The frequency of the counter on current systems. */
#define TB_WIN32_USUAL_COUNTS_PER_SECOND UINT64_C(10000000)

/* The highest counter frequency that can be converted: beyond it, the part of
 * a count that is less than a second no longer fits in 64 bits once it is
 * multiplied into nanoseconds. It is above 18 GHz. */
#define TB_WIN32_COUNTER_FREQUENCY_MAX (UINT64_MAX / TB_WIN32_NANOSECONDS_PER_SECOND)

/* Converts a count of the performance counter to nanoseconds, rounded down.
 * `counts_per_second` is from 1 to TB_WIN32_COUNTER_FREQUENCY_MAX. The result
 * is exact for every count whose nanoseconds fit in 64 bits, and wraps around
 * beyond that. */
uint64_t tb_win32_counter_to_nanoseconds(uint64_t count, uint64_t counts_per_second);

#endif
