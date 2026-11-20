/* Shared libraries through LoadLibraryExW. Every load is by full path: the
 * system directory, the executable's, or the caller's. A module of the same
 * name that is already loaded from elsewhere is then never taken for the
 * one meant, and no search reaches the working directory or PATH. The
 * libraries the loaded one depends on come from beside it or from the
 * system directory. */
#define WIN32_LEAN_AND_MEAN
#include "../shared/library.h"
#include "../shared/fs.h"
#include "badger/assert.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Where a library's own dependencies are found. */
#define TB_WIN32_LIBRARY_FLAGS (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)

/* Room for the system's text of an error, in UTF-16 and in UTF-8, where a
 * character takes up to three bytes, and the code after it. */
#define TB_WIN32_REASON_WIDE 256
#define TB_WIN32_REASON_CAPACITY (TB_WIN32_REASON_WIDE * 3 + 32)

/* Writes " (error <code>)" at `text`, which has room for it, and
 * terminates it. */
static void tb_win32_library_code(char *text, DWORD code)
{
    char digits[16] = {0};
    size_t count = 0;
    size_t used = 0;
    DWORD rest = code;

    do
    {
        digits[count] = (char)('0' + rest % 10);
        count++;
        rest /= 10;
    } while (rest != 0);
    memcpy(text, " (error ", 8);
    used = 8;
    while (count > 0)
    {
        count--;
        text[used] = digits[count];
        used++;
    }
    text[used] = ')';
    text[used + 1] = '\0';
}

/* The system's text of an error code, as UTF-8 without its line break,
 * followed by the code: "The specified module could not be found. (error
 * 126)". `reason` has TB_WIN32_REASON_CAPACITY bytes. */
static void tb_win32_library_reason(DWORD code, char *reason)
{
    WCHAR wide[TB_WIN32_REASON_WIDE] = {0};
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                  code, 0, wide, TB_WIN32_REASON_WIDE, NULL);
    int written = 0;

    while (length > 0 &&
           (wide[length - 1] == L'\r' || wide[length - 1] == L'\n' || wide[length - 1] == L' '))
    {
        length--;
    }
    if (length > 0)
    {
        written = WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, reason,
                                      TB_WIN32_REASON_WIDE * 3, NULL, NULL);
    }
    /* coverage-off: the system has text for every code that loading sets */
    if (written <= 0)
    {
        memcpy(reason, "system error", 13);
        written = 12;
    }
    /* coverage-on */
    tb_win32_library_code(reason + written, code);
}

/* UTF-16 of a UTF-8 path, from malloc, with '/' made '\\', which a path
 * loaded with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires. A final component
 * with no extension gets a '.', or the system would add ".dll" to it. NULL
 * when there is no memory or the text is not UTF-8. */
static WCHAR *tb_win32_library_path(const char *path)
{
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    WCHAR *wide = NULL;
    bool extension = false;
    size_t length = 0;
    size_t capacity = 0;

    /* coverage-off: the name was checked for UTF-8 before it came here */
    if (needed <= 0)
    {
        return NULL;
    }
    /* coverage-on */
    /* The characters and their terminator, and the '.' that a missing
     * extension needs. */
    length = (size_t)needed - 1;
    capacity = length + 2;
    wide = malloc(capacity * sizeof(WCHAR));
    /* coverage-off: allocation fails only when the system is out of memory */
    if (wide == NULL)
    {
        return NULL;
    }
    /* coverage-on */
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, needed);
    for (size_t index = 0; index < length; index++)
    {
        if (wide[index] == L'/')
        {
            wide[index] = L'\\';
        }
        if (wide[index] == L'\\')
        {
            extension = false;
        }
        if (wide[index] == L'.')
        {
            extension = true;
        }
    }
    if (!extension)
    {
        TB_ASSERT(length + 1 < capacity);
        wide[length] = L'.';
        wide[length + 1] = L'\0';
    }
    return wide;
}

/* Whether a path is absolute: "C:\", "C:/", or a UNC "\\server". */
static bool tb_win32_library_absolute(const char *path)
{
    const bool drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                       path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    const bool network = (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
    return drive || network;
}

/* The system directory, with a separator after it, as UTF-8 from malloc;
 * NULL when there is no memory. */
static char *tb_win32_library_system_directory(void)
{
    WCHAR wide[MAX_PATH + 1] = {0};
    const UINT length = GetSystemDirectoryW(wide, MAX_PATH);
    int needed = 0;
    char *directory = NULL;

    /* coverage-off: the system directory always has a name, and a short one */
    if (length == 0 || length >= MAX_PATH)
    {
        return NULL;
    }
    /* coverage-on */
    wide[length] = L'\\';
    needed = WideCharToMultiByte(CP_UTF8, 0, wide, (int)length + 1, NULL, 0, NULL, NULL);
    directory = malloc((size_t)needed + 1);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (directory == NULL)
    {
        return NULL;
    }
    /* coverage-on */
    (void)WideCharToMultiByte(CP_UTF8, 0, wide, (int)length + 1, directory, needed, NULL, NULL);
    directory[needed] = '\0';
    return directory;
}

/* A directory and a file name joined, from malloc; NULL when there is no
 * memory. The directory ends in a separator. */
static char *tb_win32_library_join(const char *directory, const char *name)
{
    const size_t first = strlen(directory);
    const size_t second = strlen(name);
    char *path = malloc(first + second + 1);

    /* coverage-off: allocation fails only when the system is out of memory */
    if (path == NULL)
    {
        return NULL;
    }
    /* coverage-on */
    memcpy(path, directory, first);
    memcpy(path + first, name, second + 1);
    return path;
}

/* The full path that a name and a search load from, from malloc. */
static tb_library_result tb_win32_library_resolve(const char *name, tb_library_search search,
                                                  char **path, tb_library_error *error)
{
    char *directory = NULL;

    *path = NULL;
    if (search == TB_LIBRARY_SEARCH_PATH)
    {
        if (!tb_native_library_is_text(name))
        {
            (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
            tb_native_library_say(error, "not UTF-8");
            return TB_LIBRARY_INVALID;
        }
        if (!tb_win32_library_absolute(name))
        {
            (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
            tb_native_library_say(error, "not an absolute path");
            return TB_LIBRARY_INVALID;
        }
        *path = tb_win32_library_join("", name);
        goto done;
    }
    if (!tb_native_library_is_file_name(name))
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not a file name");
        return TB_LIBRARY_INVALID;
    }
    if (search == TB_LIBRARY_SEARCH_SYSTEM)
    {
        directory = tb_win32_library_system_directory();
    }
    else
    {
        /* coverage-off: the executable's directory is always known */
        if (tb_native_fs_location(TB_FS_EXECUTABLE, &directory) != TB_FS_OK)
        {
            directory = NULL;
        }
        /* coverage-on */
    }
    if (directory != NULL)
    {
        *path = tb_win32_library_join(directory, name);
        free(directory);
    }
done:
    /* coverage-off: allocation fails only when the system is out of memory */
    if (*path == NULL)
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_NO_MEMORY, name);
        tb_native_library_say(error, "out of memory");
        return TB_LIBRARY_NO_MEMORY;
    }
    /* coverage-on */
    return TB_LIBRARY_OK;
}

tb_library_result tb_library_open(const char *name, tb_library_search search, tb_library **library,
                                  tb_library_error *error)
{
    tb_library_result result = TB_LIBRARY_OK;
    tb_library *record = NULL;
    char *path = NULL;
    WCHAR *wide = NULL;
    HMODULE module = NULL;
    DWORD previous_mode = 0;
    DWORD code = 0;
    char reason[TB_WIN32_REASON_CAPACITY] = {0};

    TB_ASSERT(name != NULL);
    TB_ASSERT(library != NULL);
    TB_ASSERT(search == TB_LIBRARY_SEARCH_SYSTEM || search == TB_LIBRARY_SEARCH_APPLICATION ||
              search == TB_LIBRARY_SEARCH_PATH);
    *library = NULL;
    result = tb_win32_library_resolve(name, search, &path, error);
    if (result != TB_LIBRARY_OK)
    {
        return result;
    }
    record = tb_native_library_record(name);
    wide = tb_win32_library_path(path);
    /* coverage-off: allocation fails only when the system is out of memory;
     * the path is UTF-8, which the checks of the name made sure of */
    if (record == NULL || wide == NULL)
    {
        result = tb_native_library_fail(error, TB_LIBRARY_NO_MEMORY, name);
        tb_native_library_say(error, "out of memory");
        goto cleanup;
    }
    /* coverage-on */
    /* A library that is not one, or is damaged, must not stop the program
     * with a dialog box: the caller hears of it as an error. */
    (void)SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    module = LoadLibraryExW(wide, NULL, TB_WIN32_LIBRARY_FLAGS);
    code = GetLastError();
    (void)SetThreadErrorMode(previous_mode, NULL);
    if (module == NULL)
    {
        tb_win32_library_reason(code, reason);
        /* coverage-off: the system runs out of memory only rarely */
        if (code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY)
        {
            result = tb_native_library_fail(error, TB_LIBRARY_NO_MEMORY, name);
            tb_native_library_say(error, reason);
            goto cleanup;
        }
        /* coverage-on */
        (void)tb_native_library_not_loaded(error, name, search, NULL);
        if (search == TB_LIBRARY_SEARCH_APPLICATION)
        {
            tb_native_library_say(error, " (");
            tb_native_library_say(error, path);
            tb_native_library_say(error, ")");
        }
        tb_native_library_say(error, ": ");
        tb_native_library_say(error, reason);
        result = TB_LIBRARY_UNAVAILABLE;
        goto cleanup;
    }
    record->handle = module;
    *library = record;
    record = NULL;
    result = tb_native_library_succeed(error);
cleanup:
    free(record);
    free(wide);
    free(path);
    return result;
}

void tb_library_close(tb_library *library)
{
    if (library == NULL)
    {
        return;
    }
    (void)FreeLibrary((HMODULE)library->handle);
    free(library);
}

/* The address of an export, or NULL. */
static FARPROC tb_win32_library_find(const tb_library *library, const char *name)
{
    TB_ASSERT(library != NULL);
    TB_ASSERT(name != NULL);
    return GetProcAddress((HMODULE)library->handle, name);
}

tb_library_result tb_library_find_function(const tb_library *library, const char *name,
                                           tb_library_function *function, tb_library_error *error)
{
    const FARPROC address = tb_win32_library_find(library, name);

    TB_ASSERT(function != NULL);
    *function = NULL;
    if (address == NULL)
    {
        return tb_native_library_missing(error, library, name);
    }
    /* One function pointer type to another; the caller converts it again
     * to the function's real type. */
    *function = (tb_library_function)address;
    return tb_native_library_succeed(error);
}

tb_library_result tb_library_find_data(const tb_library *library, const char *name, void **data,
                                       tb_library_error *error)
{
    const FARPROC address = tb_win32_library_find(library, name);

    TB_ASSERT(data != NULL);
    *data = NULL;
    if (address == NULL)
    {
        return tb_native_library_missing(error, library, name);
    }
    /* GetProcAddress gives every export the type of a function, data
     * included. Windows represents both pointers alike; standard C has no
     * conversion between them, so the bytes are copied. */
    TB_STATIC_ASSERT(sizeof(*data) == sizeof(address), "data and function pointers differ in size");
    memcpy(data, &address, sizeof(*data));
    return tb_native_library_succeed(error);
}
