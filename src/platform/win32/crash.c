#include "badger/crash.h"

#include "../shared/crash_text.h"
#include "badger/fatal.h"
#include "badger/stack.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

/* Stack kept back for the report when the thread that called
 * tb_crash_catch() runs out of its own. */
#define TB_WIN32_CRASH_STACK_GUARANTEE ((ULONG)64 * 1024)

/* What an exception code means, for the codes a program meets. */
static const char *tb_win32_exception_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "misaligned data";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "floating-point division by zero";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "invalid floating-point operation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR:
        return "page could not be read in";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW:
        return "integer overflow";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_BREAKPOINT:
        return "breakpoint";
    default:
        return "unhandled exception";
    }
}

/* What an access violation tried to do with its address. */
static const char *tb_win32_access_name(ULONG_PTR operation)
{
    switch (operation)
    {
    case 0:
        return "reading";
    case 1:
        return "writing";
    case 8:
        return "executing";
    default:
        return "using";
    }
}

/* The stack of the code that raised the exception, from its own registers:
 * the stack the filter runs on starts in the system's dispatcher. */
static size_t tb_win32_crash_stack(const CONTEXT *raised, void **frames, size_t capacity)
{
#if defined(_M_X64)
    CONTEXT context = *raised;
    size_t count = 0;

    while (count < capacity && context.Rip != 0)
    {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = NULL;

        frames[count] = (void *)(uintptr_t)context.Rip;
        count++;
        function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (function == NULL)
        {
            /* A leaf function keeps no frame: it returns to the address on
             * top of the stack. */
            context.Rip = *(const DWORD64 *)(uintptr_t)context.Rsp;
            context.Rsp += sizeof(DWORD64);
        }
        else
        {
            PVOID handler_data = NULL;
            DWORD64 establisher_frame = 0;

            (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                                   &handler_data, &establisher_frame, NULL);
        }
    }
    return count;
#else
    /* Elsewhere the filter's own stack, which reaches the code that raised
     * the exception through the system's dispatcher. */
    (void)raised;
    return tb_stack_capture(frames, capacity, 0);
#endif
}

static LONG WINAPI tb_win32_crash_filter(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *const record = pointers->ExceptionRecord;
    char message[TB_FATAL_MESSAGE_CAPACITY] = {0};
    tb_crash_text line = {message, sizeof(message), 0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    tb_crash_text_add(&line, "crash: ");
    tb_crash_text_add(&line, tb_win32_exception_name(record->ExceptionCode));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        tb_crash_text_add(&line, " ");
        tb_crash_text_add(&line, tb_win32_access_name(record->ExceptionInformation[0]));
        tb_crash_text_add(&line, " ");
        tb_crash_text_add_address(&line, record->ExceptionInformation[1]);
    }
    else
    {
        tb_crash_text_add(&line, " (code ");
        tb_crash_text_add_address(&line, record->ExceptionCode);
        tb_crash_text_add(&line, ")");
    }
    tb_crash_text_add(&line, ", at ");
    tb_crash_text_add_address(&line, (uintptr_t)record->ExceptionAddress);
    frame_count = tb_win32_crash_stack(pointers->ContextRecord, frames, TB_FATAL_FRAMES_MAX);
    tb_fatal_with_stack(message, frames, frame_count);
}

void tb_crash_report_abort(int signal_number)
{
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    (void)signal_number;
    /* The stack runs from here through the C library's abort() to its
     * caller; this function is left out. */
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 1);
    tb_fatal_with_stack("crash: abort", frames, frame_count);
}

void tb_crash_catch(void)
{
    ULONG guarantee = TB_WIN32_CRASH_STACK_GUARANTEE;

    /* abort() would first open a window, in a debug build, or ask the system
     * to report the fault; the report here replaces both. */
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    if (signal(SIGABRT, tb_crash_report_abort) == SIG_ERR)
    {
        tb_fatal_message("crash: the C library refused a handler for abort()");
    }
    (void)SetUnhandledExceptionFilter(tb_win32_crash_filter);
    /* Best effort: without it, running out of stack is reported only when
     * what the system leaves is enough. */
    (void)SetThreadStackGuarantee(&guarantee);
}
