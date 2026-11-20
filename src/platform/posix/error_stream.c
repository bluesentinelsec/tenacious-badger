#include "badger/platform.h"

#include "badger/assert.h"

#include <errno.h>
#include <unistd.h>

void tb_platform_write_error(const void *bytes, size_t length)
{
    const unsigned char *next = bytes;

    TB_ASSERT(bytes != NULL || length == 0);
    while (length > 0)
    {
        const ssize_t written = write(STDERR_FILENO, next, length);

        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            return;
        }
        next += written;
        length -= (size_t)written;
    }
}
