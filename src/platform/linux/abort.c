#include "badger/platform.h"

#include <stdlib.h>

#if defined(TB_COVERAGE)
/* From the compiler's coverage runtime. A process writes the counts of the
 * lines it ran when it exits, and this one is about to end without exiting. */
void __gcov_dump(void);
#endif

void tb_platform_abort(void)
{
#if defined(TB_COVERAGE)
    __gcov_dump();
#endif
    /* coverage-off: the process ends on this line, after its counts were written */
    /* SIGABRT stops an attached debugger, leaves a core dump or crash report
     * where the system is configured for one, and never prompts. */
    abort();
    /* coverage-on */
}
