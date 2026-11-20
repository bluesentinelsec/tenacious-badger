/* The machine state of a signal context is not part of POSIX. */
#define _DARWIN_C_SOURCE

#include "../posix/crash_context.h"

#include <stdint.h>
#include <sys/ucontext.h>

void *tb_posix_crash_instruction(const void *context)
{
    const ucontext_t *const raised = context;

#if defined(__x86_64__)
    return (void *)(uintptr_t)raised->uc_mcontext->__ss.__rip;
#elif defined(__arm64__)
    return (void *)(uintptr_t)__darwin_arm_thread_state64_get_pc(raised->uc_mcontext->__ss);
#else
    (void)raised;
    return NULL;
#endif
}
