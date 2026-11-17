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
    /* The one function the default adapter implements, because reporting
     * "not implemented" has to end the process, and standard C can. _Exit
     * runs no exit handlers, as the interface promises, and unlike abort()
     * it never waits on an error dialog. A debugger does not stop here: a
     * platform's own adapter adds that. */
    _Exit(EXIT_FAILURE);
}
