/* The process's arguments, read from the system as UTF-16 rather than from
 * the C runtime, which converts them to the ANSI code page. */
#include "process_arguments.h"

#include "badger/assert.h"

#include <limits.h>
#include <shellapi.h>
#include <stdint.h>
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
