/* Random bytes from arc4random_buf, which macOS backs with the kernel's
 * generator and which cannot fail. */
#include "badger/random.h"

#include "badger/assert.h"

#include <stdlib.h>

bool tb_random_bytes(void *bytes, size_t length)
{
    TB_ASSERT(bytes != NULL || length == 0);
    if (length > 0)
    {
        arc4random_buf(bytes, length);
    }
    return true;
}
