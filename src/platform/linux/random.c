/* Random bytes from getrandom, which takes them from the kernel's generator
 * and waits only until that generator has been seeded once at boot. */
#include "badger/random.h"

#include "badger/assert.h"

#include <errno.h>
#include <sys/random.h>
#include <sys/types.h>

bool tb_random_bytes(void *bytes, size_t length)
{
    unsigned char *output = bytes;
    TB_ASSERT(bytes != NULL || length == 0);
    while (length > 0)
    {
        /* A call gives at most 32 MiB less one byte, and a signal can cut it short. */
        const ssize_t count = getrandom(output, length, 0);
        if (count < 0)
        {
            /* coverage-off: only a signal interrupts it, and nothing else fails */
            if (errno == EINTR)
            {
                continue;
            }
            return false;
            /* coverage-on */
        }
        output += count;
        length -= (size_t)count;
    }
    return true;
}
