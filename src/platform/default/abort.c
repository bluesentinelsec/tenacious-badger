#include "badger/platform.h"

#include <stdlib.h>

void tb_platform_abort(void)
{
    /* The one function the default adapter implements, because reporting
     * "not implemented" has to end the process, and standard C can. _Exit
     * runs no exit handlers, as the interface promises, and unlike abort()
     * it never waits on an error dialog. A debugger does not stop here: a
     * platform's own adapter adds that. */
    _Exit(EXIT_FAILURE);
}
