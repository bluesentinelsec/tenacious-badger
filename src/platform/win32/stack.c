#include "badger/stack.h"

#include "badger/assert.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* The declarations of the debugging library. The library itself is loaded
 * when the first address is described, so a program that never asks pays
 * nothing for it. */
#include <dbghelp.h>

#include <string.h>

/* The most frames that one capture asks the system for. */
#define TB_WIN32_STACK_FRAMES_MAX ((size_t)128)

/* Characters of a symbol's name that are asked for. */
#define TB_WIN32_SYMBOL_NAME_CAPACITY 512

size_t tb_stack_capture(void **frames, size_t capacity, size_t skip)
{
    size_t wanted = capacity;

    TB_ASSERT(frames != NULL || capacity == 0);
    if (wanted > TB_WIN32_STACK_FRAMES_MAX)
    {
        wanted = TB_WIN32_STACK_FRAMES_MAX;
    }
    if (wanted == 0 || skip >= TB_WIN32_STACK_FRAMES_MAX)
    {
        return 0;
    }
    /* One more is left out than was asked: this function. */
    return RtlCaptureStackBackTrace((ULONG)skip + 1, (ULONG)wanted, frames, NULL);
}

/* ---------------------------------------------------------------------------
 * The debugging library, DbgHelp. It is not safe to call from two threads, so
 * one lock serializes everything done with it.
 * ------------------------------------------------------------------------- */

typedef BOOL(WINAPI tb_win32_sym_initialize)(HANDLE process, PCWSTR search_path, BOOL invade);
typedef DWORD(WINAPI tb_win32_sym_set_options)(DWORD options);
typedef BOOL(WINAPI tb_win32_sym_from_address)(HANDLE process, DWORD64 address,
                                               PDWORD64 displacement, PSYMBOL_INFOW symbol);
typedef BOOL(WINAPI tb_win32_sym_line_from_address)(HANDLE process, DWORD64 address,
                                                    PDWORD displacement, PIMAGEHLP_LINEW64 line);

typedef struct tb_win32_debug_library
{
    SRWLOCK lock;       /* Held for every call into the library. */
    bool was_attempted; /* Loading was tried, whether or not it worked. */
    bool is_ready;      /* The functions below can be called. */
    tb_win32_sym_from_address *from_address;
    tb_win32_sym_line_from_address *line_from_address;
} tb_win32_debug_library;

static tb_win32_debug_library tb_win32_debug = {.lock = SRWLOCK_INIT};

/* Set while the calling thread holds the library's lock. A thread that
 * crashes in the library, and then describes its own stack, would otherwise
 * wait for itself. */
static _Thread_local bool tb_win32_debug_is_held = false;

/* Loads the library and the program's symbols, once. The caller holds the
 * lock. A system without the library, or a function it lacks, leaves names
 * unknown; that is not a failure of the program. */
static void tb_win32_debug_load(void)
{
    HMODULE library = NULL;
    tb_win32_sym_initialize *initialize = NULL;
    tb_win32_sym_set_options *set_options = NULL;

    if (tb_win32_debug.was_attempted)
    {
        return;
    }
    tb_win32_debug.was_attempted = true;
    library = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    /* coverage-off: the library is part of every Windows the engine supports */
    if (library == NULL)
    {
        return;
    }
    /* coverage-on */
    initialize = (tb_win32_sym_initialize *)(void *)GetProcAddress(library, "SymInitializeW");
    set_options = (tb_win32_sym_set_options *)(void *)GetProcAddress(library, "SymSetOptions");
    tb_win32_debug.from_address =
        (tb_win32_sym_from_address *)(void *)GetProcAddress(library, "SymFromAddrW");
    tb_win32_debug.line_from_address =
        (tb_win32_sym_line_from_address *)(void *)GetProcAddress(library, "SymGetLineFromAddrW64");
    /* coverage-off: as above */
    if (initialize == NULL || set_options == NULL || tb_win32_debug.from_address == NULL ||
        tb_win32_debug.line_from_address == NULL)
    {
        return;
    }
    /* coverage-on */
    /* Lines as well as names; symbols of a module only when one of its
     * addresses is asked about; and never a window that asks the user. */
    (void)set_options(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_NO_PROMPTS);
    tb_win32_debug.is_ready = initialize(GetCurrentProcess(), NULL, TRUE) != FALSE;
}

/* Copies text to UTF-8, cut off at a whole character when it does not fit. */
static void tb_win32_copy_text(char *destination, size_t capacity, const WCHAR *text)
{
    size_t length = wcslen(text);

    destination[0] = '\0';
    /* Shorter and shorter, until what is left converts into the room there is. */
    while (length > 0)
    {
        const int written = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, destination,
                                                (int)(capacity - 1), NULL, NULL);

        if (written > 0)
        {
            destination[written] = '\0';
            return;
        }
        length--;
    }
}

/* The module that an address belongs to: its file name, and how far into it
 * the address is. */
static void tb_win32_describe_module(const void *address, tb_stack_location *location)
{
    WCHAR path[MAX_PATH] = {0};
    const WCHAR *name = path;
    HMODULE module = NULL;
    DWORD length = 0;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            address, &module) ||
        module == NULL)
    {
        return;
    }
    location->module_offset = (size_t)((uintptr_t)address - (uintptr_t)module);
    length = GetModuleFileNameW(module, path, MAX_PATH);
    /* coverage-off: a loaded module has a file name, and one longer than fits is cut off */
    if (length == 0)
    {
        return;
    }
    /* coverage-on */
    path[MAX_PATH - 1] = L'\0';
    for (const WCHAR *character = path; *character != L'\0'; character++)
    {
        if (*character == L'\\' || *character == L'/')
        {
            name = character + 1;
        }
    }
    tb_win32_copy_text(location->module, sizeof(location->module), name);
}

/* The function, file, and line of an address, where the program's debugging
 * information says. */
static void tb_win32_describe_symbol(const void *address, tb_stack_location *location)
{
    /* A symbol's name follows its structure in memory. */
    union {
        SYMBOL_INFOW symbol;
        WCHAR room[sizeof(SYMBOL_INFOW) / sizeof(WCHAR) + TB_WIN32_SYMBOL_NAME_CAPACITY];
    } storage = {0};
    IMAGEHLP_LINEW64 line = {.SizeOfStruct = sizeof(IMAGEHLP_LINEW64)};
    const DWORD64 value = (DWORD64)(uintptr_t)address;
    DWORD64 function_offset = 0;
    DWORD line_offset = 0;

    storage.symbol.SizeOfStruct = sizeof(SYMBOL_INFOW);
    storage.symbol.MaxNameLen = TB_WIN32_SYMBOL_NAME_CAPACITY;
    /* coverage-off: needs a crash inside the debugging library */
    if (tb_win32_debug_is_held)
    {
        return;
    }
    /* coverage-on */
    AcquireSRWLockExclusive(&tb_win32_debug.lock);
    tb_win32_debug_is_held = true;
    tb_win32_debug_load();
    if (tb_win32_debug.is_ready)
    {
        const HANDLE process = GetCurrentProcess();

        if (tb_win32_debug.from_address(process, value, &function_offset, &storage.symbol))
        {
            tb_win32_copy_text(location->function, sizeof(location->function), storage.symbol.Name);
            location->function_offset = (size_t)function_offset;
        }
        if (tb_win32_debug.line_from_address(process, value, &line_offset, &line))
        {
            tb_win32_copy_text(location->file, sizeof(location->file), line.FileName);
            location->line = line.LineNumber;
        }
    }
    tb_win32_debug_is_held = false;
    ReleaseSRWLockExclusive(&tb_win32_debug.lock);
}

void tb_stack_describe(const void *address, tb_stack_location *location)
{
    TB_ASSERT(location != NULL);
    memset(location, 0, sizeof(*location));
    location->address = address;
    if (address == NULL)
    {
        return;
    }
    tb_win32_describe_module(address, location);
    tb_win32_describe_symbol(address, location);
}
