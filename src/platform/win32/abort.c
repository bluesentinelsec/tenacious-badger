#include "badger/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* Exit status of an aborted process; the same value the C runtime's abort() uses. */
#define TB_PLATFORM_ABORT_EXIT_STATUS 3u

void tb_platform_abort(void)
{
    /* coverage-off: the break is reached only with a debugger attached */
    if (IsDebuggerPresent())
    {
        DebugBreak();
    }
    /* coverage-on */
    /* The C runtime's abort() is not used: in debug builds it opens a modal
     * error window, which would park an unattended run until it times out. */
    /* coverage-off: the process ends on these lines, before a collector can count them */
    (void)TerminateProcess(GetCurrentProcess(), TB_PLATFORM_ABORT_EXIT_STATUS);
    ExitProcess(TB_PLATFORM_ABORT_EXIT_STATUS);
    /* coverage-on */
}
