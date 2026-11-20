#include "badger/crash.h"

#include "../shared/crash_text.h"
#include "badger/fatal.h"
#include "badger/stack.h"
#include "debug_library.h"

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Stack kept back for the report when the thread that called
 * tb_crash_catch() runs out of its own. */
#define TB_WIN32_CRASH_STACK_GUARANTEE ((ULONG)64 * 1024)

/* Characters of a dump's path, its terminator included. */
#define TB_WIN32_DUMP_PATH_CAPACITY ((size_t)1024)

/* How long a crashed thread waits for its dump, in milliseconds, before it
 * reports the crash without one. */
#define TB_WIN32_DUMP_WAIT_MS ((DWORD)60 * 1000)

/* ---------------------------------------------------------------------------
 * Crash dumps. A thread cannot reliably dump itself, so a thread of the
 * engine's, started when dumps are asked for, waits to write one, and the
 * crashed thread hands it the exception and waits until it is done. Only the
 * first crash is dumped: the process ends after it.
 * ------------------------------------------------------------------------- */

typedef struct tb_win32_dump
{
    WCHAR path[TB_WIN32_DUMP_PATH_CAPACITY];         /* Where the dump goes. */
    char path_text[TB_WIN32_DUMP_PATH_CAPACITY * 3]; /* The same, in UTF-8, for the report. */
    HANDLE requested;             /* Set by the crashed thread when the fields below are ready. */
    HANDLE finished;              /* Set by the writer when it is done. */
    LONG claimed;                 /* 1 once a crash has asked for the dump. */
    DWORD thread_id;              /* The thread that crashed. */
    EXCEPTION_POINTERS *pointers; /* Its exception, or NULL for abort(). */
    bool was_written;             /* Whether the writer wrote it. */
} tb_win32_dump;

static tb_win32_dump tb_win32_crash_dump = {0};

static DWORD WINAPI tb_win32_dump_writer(LPVOID context)
{
    tb_win32_dump *const dump = context;
    HANDLE file = INVALID_HANDLE_VALUE;

    (void)WaitForSingleObject(dump->requested, INFINITE);
    file =
        CreateFileW(dump->path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        dump->was_written = tb_win32_debug_write_dump(file, dump->thread_id, dump->pointers);
        (void)CloseHandle(file);
    }
    (void)SetEvent(dump->finished);
    return 0;
}

/* Has the crash dumped, where dumps were asked for, and says on the report's
 * line where the dump is. */
static void tb_win32_dump_crash(EXCEPTION_POINTERS *pointers, tb_crash_text *line)
{
    tb_win32_dump *const dump = &tb_win32_crash_dump;

    if (dump->requested == NULL || InterlockedCompareExchange(&dump->claimed, 1, 0) != 0)
    {
        return;
    }
    dump->thread_id = GetCurrentThreadId();
    dump->pointers = pointers;
    /* Setting the event orders the fields above before the writer reads
     * them; waiting for the other orders its result before this reads it. */
    (void)SetEvent(dump->requested);
    if (WaitForSingleObject(dump->finished, TB_WIN32_DUMP_WAIT_MS) == WAIT_OBJECT_0 &&
        dump->was_written)
    {
        tb_crash_text_add(line, "; dump: ");
        tb_crash_text_add(line, dump->path_text);
        return;
    }
    /* coverage-off: a dump fails only where the disk does */
    tb_crash_text_add(line, "; no dump could be written to ");
    tb_crash_text_add(line, dump->path_text);
    /* coverage-on */
}

/* The directory a dump goes in: the one given, or the system's temporary
 * directory. Its path ends with a separator. Returns its length, or 0 where
 * it cannot be used. */
static size_t tb_win32_dump_directory(const char *directory, WCHAR *path, size_t capacity)
{
    size_t length = 0;
    DWORD attributes = 0;

    if (directory == NULL)
    {
        length = GetTempPathW((DWORD)capacity, path);
        if (length == 0 || length >= capacity)
        {
            return 0;
        }
    }
    else
    {
        const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1,
                                                  path, (int)capacity - 1);
        if (converted <= 1)
        {
            return 0;
        }
        length = (size_t)converted - 1;
        if (path[length - 1] != L'\\' && path[length - 1] != L'/')
        {
            path[length] = L'\\';
            length++;
            path[length] = L'\0';
        }
    }
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return 0;
    }
    return length;
}

/* Appends text to a name being built, while it fits with its terminator. */
static bool tb_win32_dump_name_add(WCHAR *name, size_t capacity, size_t *length, const WCHAR *text,
                                   size_t text_length)
{
    if (text_length >= capacity - *length)
    {
        return false;
    }
    (void)memcpy(name + *length, text, text_length * sizeof(WCHAR));
    *length += text_length;
    name[*length] = L'\0';
    return true;
}

/* The dump's file name: the program's, without its extension, and the
 * process's number, as in `burrow-1234.dmp`. */
static bool tb_win32_dump_file_name(WCHAR *name, size_t capacity)
{
    WCHAR program[MAX_PATH] = {0};
    WCHAR digits[16] = {0};
    const WCHAR *base = program;
    const WCHAR *end = NULL;
    DWORD process = GetCurrentProcessId();
    size_t digit_count = 0;
    size_t length = 0;
    const DWORD program_length = GetModuleFileNameW(NULL, program, MAX_PATH);

    /* coverage-off: the program's own path is always known */
    if (program_length == 0 || program_length >= MAX_PATH)
    {
        return false;
    }
    /* coverage-on */
    end = program + program_length;
    for (const WCHAR *each = program; each < end; each++)
    {
        if (*each == L'\\' || *each == L'/')
        {
            base = each + 1;
        }
    }
    /* The extension starts at the last dot. */
    for (const WCHAR *each = end; each > base; each--)
    {
        if (each[-1] == L'.')
        {
            end = each - 1;
            break;
        }
    }
    /* The number's digits, last first. */
    do
    {
        digits[digit_count] = (WCHAR)(L'0' + process % 10);
        digit_count++;
        process /= 10;
    } while (process != 0);
    for (size_t index = 0; index < digit_count / 2; index++)
    {
        const WCHAR swapped = digits[index];
        digits[index] = digits[digit_count - 1 - index];
        digits[digit_count - 1 - index] = swapped;
    }
    return tb_win32_dump_name_add(name, capacity, &length, base, (size_t)(end - base)) &&
           tb_win32_dump_name_add(name, capacity, &length, L"-", 1) &&
           tb_win32_dump_name_add(name, capacity, &length, digits, digit_count) &&
           tb_win32_dump_name_add(name, capacity, &length, L".dmp", 4);
}

bool tb_crash_write_dumps(const char *directory)
{
    tb_win32_dump *const dump = &tb_win32_crash_dump;
    WCHAR path[TB_WIN32_DUMP_PATH_CAPACITY] = {0};
    size_t length = 0;
    int converted = 0;
    HANDLE writer = NULL;

    length = tb_win32_dump_directory(directory, path, TB_WIN32_DUMP_PATH_CAPACITY);
    if (length == 0 ||
        !tb_win32_dump_file_name(path + length, TB_WIN32_DUMP_PATH_CAPACITY - length) ||
        !tb_win32_debug_can_write_dump())
    {
        return false;
    }
    converted = WideCharToMultiByte(CP_UTF8, 0, path, -1, dump->path_text,
                                    (int)sizeof(dump->path_text), NULL, NULL);
    /* coverage-off: three bytes of UTF-8 for each character always fit */
    if (converted <= 0)
    {
        return false;
    }
    /* coverage-on */
    (void)memcpy(dump->path, path, sizeof(dump->path));
    if (dump->requested != NULL)
    {
        return true;
    }
    dump->requested = CreateEventW(NULL, FALSE, FALSE, NULL);
    dump->finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* coverage-off: the system refuses an event or a thread only when out of resources */
    if (dump->requested == NULL || dump->finished == NULL)
    {
        tb_fatal_message("crash: the system refused an event for crash dumps");
    }
    writer = CreateThread(NULL, 0, tb_win32_dump_writer, dump, 0, NULL);
    if (writer == NULL)
    {
        tb_fatal_message("crash: the system refused a thread for crash dumps");
    }
    /* coverage-on */
    (void)CloseHandle(writer);
    return true;
}

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
    tb_win32_dump_crash(pointers, &line);
    frame_count = tb_win32_crash_stack(pointers->ContextRecord, frames, TB_FATAL_FRAMES_MAX);
    tb_fatal_with_stack(message, frames, frame_count);
}

void tb_crash_report_abort(int signal_number)
{
    char message[TB_FATAL_MESSAGE_CAPACITY] = {0};
    tb_crash_text line = {message, sizeof(message), 0};
    void *frames[TB_FATAL_FRAMES_MAX] = {0};
    size_t frame_count = 0;

    (void)signal_number;
    tb_crash_text_add(&line, "crash: abort");
    tb_win32_dump_crash(NULL, &line);
    /* The stack runs from here through the C library's abort() to its
     * caller; this function is left out. */
    frame_count = tb_stack_capture(frames, TB_FATAL_FRAMES_MAX, 1);
    tb_fatal_with_stack(message, frames, frame_count);
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
