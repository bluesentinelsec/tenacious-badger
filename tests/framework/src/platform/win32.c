#include "../platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char *tb_test_crash_message = NULL;
static DWORD tb_test_crash_length = {0};
static LPTOP_LEVEL_EXCEPTION_FILTER tb_test_previous_filter = NULL;
static const int tb_test_fatal_signals[] = {SIGABRT};
typedef void (*tb_test_signal_handler)(int);
static tb_test_signal_handler tb_test_saved_signals[1] = {0};
static bool tb_test_installed_signals[1] = {0};

static void tb_test_crash_write(void)
{
    DWORD written = {0};
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), tb_test_crash_message, tb_test_crash_length,
                    &written, NULL);
}
static LONG WINAPI tb_test_crash_exception(EXCEPTION_POINTERS *exception)
{
    tb_test_crash_write();
    if (tb_test_previous_filter != NULL)
    {
        return tb_test_previous_filter(exception);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void tb_test_crash_signal(int number)
{
    tb_test_crash_write();
    (void)signal(number, SIG_DFL);
    (void)raise(number);
}
void tb_test_platform_crash_begin(const char *message, size_t length)
{
    tb_test_crash_message = message;
    tb_test_crash_length = (DWORD)length;
    tb_test_previous_filter = SetUnhandledExceptionFilter(tb_test_crash_exception);
    for (size_t index = {0}; index < 1; index++)
    {
        tb_test_saved_signals[index] = signal(tb_test_fatal_signals[index], tb_test_crash_signal);
        tb_test_installed_signals[index] = tb_test_saved_signals[index] == SIG_DFL;
        if (!tb_test_installed_signals[index] && tb_test_saved_signals[index] != SIG_ERR)
        {
            (void)signal(tb_test_fatal_signals[index], tb_test_saved_signals[index]);
        }
    }
}
void tb_test_platform_crash_end(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER current = NULL;
    current = SetUnhandledExceptionFilter(tb_test_previous_filter);
    if (current != tb_test_crash_exception)
    {
        (void)SetUnhandledExceptionFilter(current);
    }
    for (size_t index = {0}; index < 1; index++)
    {
        if (tb_test_installed_signals[index])
        {
            tb_test_signal_handler previous = NULL;
            previous = signal(tb_test_fatal_signals[index], tb_test_saved_signals[index]);
            if (previous != tb_test_crash_signal && previous != SIG_ERR)
            {
                (void)signal(tb_test_fatal_signals[index], previous);
            }
        }
    }
    tb_test_crash_message = NULL;
    tb_test_crash_length = 0;
}

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
