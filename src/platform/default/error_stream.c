#include "badger/platform.h"

#include "not_implemented.h"

/* Standard C writes to stderr only through its own stream, which takes a
 * lock that a crashed thread may hold. */

void tb_platform_write_error(const void *bytes, size_t length)
{
    (void)bytes;
    (void)length;
    tb_default_not_implemented("platform.error", "tb_platform_write_error");
}
