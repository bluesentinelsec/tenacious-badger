#include "badger/platform.h"

#include "badger/assert.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* The most one call writes; WriteFile counts in 32 bits. */
#define TB_WIN32_ERROR_WRITE_MAX ((size_t)1 << 30)

void tb_platform_write_error(const void *bytes, size_t length)
{
    const unsigned char *next = bytes;
    const HANDLE stream = GetStdHandle(STD_ERROR_HANDLE);

    TB_ASSERT(bytes != NULL || length == 0);
    if (stream == NULL || stream == INVALID_HANDLE_VALUE)
    {
        return;
    }
    while (length > 0)
    {
        const DWORD piece =
            (DWORD)(length < TB_WIN32_ERROR_WRITE_MAX ? length : TB_WIN32_ERROR_WRITE_MAX);
        DWORD written = 0;

        if (!WriteFile(stream, next, piece, &written, NULL) || written == 0)
        {
            return;
        }
        next += written;
        length -= written;
    }
}
