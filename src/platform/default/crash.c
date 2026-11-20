#include "badger/crash.h"

#include "not_implemented.h"

/* Standard C can install a handler for a signal, but cannot capture the
 * stack of the code that raised it, nor say where it was. */

void tb_crash_catch(void)
{
    tb_default_not_implemented("platform.crash", "tb_crash_catch");
}

void tb_crash_report_abort(int signal_number)
{
    (void)signal_number;
    tb_default_not_implemented("platform.crash", "tb_crash_report_abort");
}

bool tb_crash_write_dumps(const char *directory)
{
    (void)directory;
    tb_default_not_implemented("platform.crash", "tb_crash_write_dumps");
}
