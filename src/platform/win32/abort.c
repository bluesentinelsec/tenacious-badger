#include "badger/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* Exit status of an aborted process; the same value the C runtime's abort() uses. */
#define TB_PLATFORM_ABORT_EXIT_STATUS 3u

void tb_platform_abort(void)
{
    if (IsDebuggerPresent())
    {
        DebugBreak();
    }
    /* The C runtime's abort() is not used: in debug builds it opens a modal
     * error window, which would park an unattended run until it times out. */
    (void)TerminateProcess(GetCurrentProcess(), TB_PLATFORM_ABORT_EXIT_STATUS);
    ExitProcess(TB_PLATFORM_ABORT_EXIT_STATUS);
}
