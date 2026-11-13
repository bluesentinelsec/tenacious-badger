#include "../platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>
#include <stdio.h>
#include <stdlib.h>

void tb_test_platform_disable_error_dialogs(void)
{
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    /* The debug C runtime reports assertions and errors in a modal window by
     * default. Send them to stderr; the report calls compile away in release. */
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}

void tb_test_platform_debug_break(void)
{
    DebugBreak();
}

void tb_test_platform_print_location(const char *file, int line)
{
    /* MSVC diagnostic form: Visual Studio and VS Code link `file(line)`. */
    printf("%s(%d)", file, line);
}
