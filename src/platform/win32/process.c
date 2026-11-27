/* The process's arguments, read from the system as UTF-16 rather than from
 * the C runtime, which converts them to the ANSI code page; and its standard
 * streams sent to a file. */
#include "process_arguments.h"

#include "badger/assert.h"

#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bytes of every argument in UTF-8, terminators included; false when
 * they would not fit in a size_t. */
static bool tb_win32_arguments_size(WCHAR *const *wide, int count, size_t *bytes)
{
    *bytes = 0;
    for (int index = 0; index < count; index++)
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, NULL, 0, NULL, NULL);
        /* coverage-off: every argument converts, with U+FFFD for a lone
         * surrogate, and none is near SIZE_MAX bytes */
        if (length <= 0 || *bytes > SIZE_MAX - (size_t)length)
        {
            return false;
        }
        /* coverage-on */
        *bytes += (size_t)length;
    }
    return true;
}

bool tb_win32_arguments_from(const WCHAR *command_line, tb_arguments *arguments)
{
    WCHAR **wide = NULL;
    int count = 0;
    size_t bytes = 0;
    size_t table = 0;
    unsigned char *storage = NULL;
    const char **values = NULL;
    char *text = NULL;
    bool done = false;

    TB_ASSERT(command_line != NULL);
    TB_ASSERT(arguments != NULL);
    wide = CommandLineToArgvW(command_line, &count);
    /* coverage-off: splitting fails only when the system is out of memory */
    if (wide == NULL || count < 1)
    {
        goto cleanup;
    }
    /* coverage-on */
    table = (size_t)count * sizeof(*values);
    /* coverage-off: as tb_win32_arguments_size() */
    if (!tb_win32_arguments_size(wide, count, &bytes) || bytes > SIZE_MAX - table)
    {
        goto cleanup;
    }
    /* coverage-on */
    storage = malloc(table + bytes);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (storage == NULL)
    {
        goto cleanup;
    }
    /* coverage-on */
    /* The table of pointers first, then the text, which needs no alignment. */
    values = (const char **)(void *)storage;
    text = (char *)(storage + table);
    for (int index = 0; index < count; index++)
    {
        const int length =
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, text,
                                (int)(bytes > INT_MAX ? INT_MAX : bytes), NULL, NULL);
        values[index] = text;
        text += length;
        bytes -= (size_t)length;
    }
    arguments->count = (size_t)count;
    arguments->values = values;
    arguments->storage = storage;
    storage = NULL;
    done = true;
cleanup:
    free(storage);
    if (wide != NULL)
    {
        (void)LocalFree(wide);
    }
    return done;
}

bool tb_process_get_arguments(int count, char **values, tb_arguments *arguments)
{
    (void)count;
    (void)values;
    TB_ASSERT(arguments != NULL);
    return tb_win32_arguments_from(GetCommandLineW(), arguments);
}

void tb_process_release_arguments(tb_arguments *arguments)
{
    TB_ASSERT(arguments != NULL);
    free(arguments->storage);
    arguments->count = 0;
    arguments->values = NULL;
    arguments->storage = NULL;
}

/* The UTF-16 of a UTF-8 path, from malloc; NULL when the text is not UTF-8. */
static WCHAR *tb_win32_output_path(const char *path)
{
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    WCHAR *wide = NULL;

    if (needed <= 0)
    {
        return NULL;
    }
    wide = malloc((size_t)needed * sizeof(WCHAR));
    /* coverage-off: allocation fails only when the system is out of memory */
    if (wide == NULL)
    {
        return NULL;
    }
    /* coverage-on */
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, needed);
    return wide;
}

/* Without an inherited handle the UCRT gives the stream a sentinel that
 * _dup2 cannot target. Reopening the FILE gives it a real descriptor. */
static int tb_win32_stream_descriptor(FILE *stream)
{
    if (_fileno(stream) < 0)
    {
        FILE *opened = NULL;
        if (_wfreopen_s(&opened, L"NUL", L"w", stream) != 0)
        {
            return -1;
        }
    }
    return _fileno(stream);
}

bool tb_process_write_output_to(const char *path)
{
    WCHAR *wide = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    int descriptor = -1;
    int output = -1;
    int error = -1;
    int saved_output = -1;
    bool moved = false;

    TB_ASSERT(path != NULL);
    wide = tb_win32_output_path(path);
    if (wide == NULL)
    {
        goto cleanup;
    }
    file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        goto cleanup;
    }
    /* The C runtime's descriptor owns the handle from here on. */
    descriptor = _open_osfhandle((intptr_t)file, _O_WRONLY | _O_TEXT);
    /* coverage-off: a descriptor is refused only when the runtime has none left */
    if (descriptor < 0)
    {
        (void)CloseHandle(file);
        goto cleanup;
    }
    /* coverage-on */
    output = tb_win32_stream_descriptor(stdout);
    error = tb_win32_stream_descriptor(stderr);
    if (output < 0 || error < 0)
    {
        goto cleanup;
    }
    /* Save stdout until stderr has moved, so a second duplication failure
     * leaves the original output available for diagnostics. */
    saved_output = _dup(output);
    if (saved_output < 0 || _dup2(descriptor, output) != 0)
    {
        goto cleanup;
    }
    if (_dup2(descriptor, error) != 0)
    {
        (void)_dup2(saved_output, output);
        goto cleanup;
    }
    /* The system's standard handles too, which a crash report writes to. */
    (void)SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    (void)SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
    moved = true;
cleanup:
    if (saved_output >= 0)
    {
        (void)_close(saved_output);
    }
    if (descriptor >= 0)
    {
        (void)_close(descriptor);
    }
    free(wide);
    return moved;
}
