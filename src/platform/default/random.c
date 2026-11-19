#include "badger/random.h"

#include "not_implemented.h"

/* Standard C has no source of randomness fit for a key: rand() is a
 * repeatable sequence, and a key made from it could be guessed. */

bool tb_random_bytes(void *bytes, size_t length)
{
    (void)bytes;
    (void)length;
    tb_default_not_implemented("platform.random", "tb_random_bytes");
}
