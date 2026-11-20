#include "badger/platform.h"

#include <signal.h>
#include <stdlib.h>

#if defined(TB_COVERAGE)
/* From the compiler's coverage runtime. A process writes the counts of the
 * lines it ran when it exits, and this one is about to end without exiting. */
void __gcov_dump(void);
#endif

void tb_platform_abort(void)
{
    /* A handler of SIGABRT, such as the one of tb_crash_catch(), would report
     * this end as a crash: whatever ends the process here has reported. */
    (void)signal(SIGABRT, SIG_DFL);
#if defined(TB_COVERAGE)
    __gcov_dump();
#endif
    /* coverage-off: the process ends on this line, after its counts were written */
    /* SIGABRT stops an attached debugger, leaves a core dump or crash report
     * where the system is configured for one, and never prompts. */
    abort();
    /* coverage-on */
}
