/* Private to the platform adapters: waiting for a moment more exactly than a
 * sleep can. Every adapter that has a clock, a sleep, and tb_thread_relax()
 * shares it. */
#ifndef TB_PLATFORM_SHARED_WAIT_H
#define TB_PLATFORM_SHARED_WAIT_H

#include <stdint.h>

/* Returns when the clock of tb_platform_clock_nanoseconds() reads `deadline`
 * or later, and at once when it already does.
 *
 * The thread sleeps until `slack` nanoseconds before the deadline, and then
 * runs, watching the clock, until the deadline. `slack` is how late the
 * adapter's sleep may wake on a machine that is not busy: the wait is then
 * as exact as the clock, at the price of a processor for that long. */
void tb_shared_wait_until(uint64_t deadline, uint64_t slack);

#endif
