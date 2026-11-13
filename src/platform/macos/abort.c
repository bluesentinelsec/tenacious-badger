#include "badger/platform.h"

#include <stdlib.h>

void tb_platform_abort(void)
{
    /* SIGABRT stops an attached debugger, leaves a core dump or crash report
     * where the system is configured for one, and never prompts. */
    abort();
}
