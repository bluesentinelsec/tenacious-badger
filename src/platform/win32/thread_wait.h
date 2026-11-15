/* Private to the Windows adapter: the arithmetic that turns a time limit in
 * nanoseconds into the milliseconds a system wait takes, apart from the wait
 * so that it can be tested with limits nobody will wait for. */
#ifndef TB_WIN32_THREAD_WAIT_H
#define TB_WIN32_THREAD_WAIT_H

#include <stdint.h>

/* The longest limit a system wait accepts. The next value means no limit. */
#define TB_WIN32_WAIT_MILLISECONDS_MAX UINT32_C(0xFFFFFFFE)

/* Milliseconds that last at least `nanoseconds`, so that a wait of them never
 * ends too soon, or TB_WIN32_WAIT_MILLISECONDS_MAX when that is less. */
uint32_t tb_win32_wait_milliseconds(uint64_t nanoseconds);

#endif
