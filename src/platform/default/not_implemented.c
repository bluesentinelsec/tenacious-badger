#include "not_implemented.h"

#include "badger/platform.h"
#include "platform_name.h"

#include <stdio.h>

void tb_default_not_implemented(const char *interface_name, const char *function_name)
{
    (void)fputs(interface_name, stderr);
    (void)fputs(": ", stderr);
    (void)fputs(function_name, stderr);
    (void)fputs(" on " TB_DEFAULT_PLATFORM_NAME
                ": not implemented; add a platform adapter for " TB_DEFAULT_PLATFORM_NAME
                " under src/platform/\n",
                stderr);
    tb_platform_abort();
}
