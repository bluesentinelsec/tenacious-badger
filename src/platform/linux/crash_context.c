/* The names of the registers in a signal context are not part of POSIX. */
#define _GNU_SOURCE

#include "../posix/crash_context.h"

#include <stdint.h>
#include <ucontext.h>

void *tb_posix_crash_instruction(const void *context)
{
    const ucontext_t *const raised = context;

#if defined(__x86_64__)
    return (void *)(uintptr_t)raised->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (void *)(uintptr_t)raised->uc_mcontext.pc;
#else
    (void)raised;
    return NULL;
#endif
}
