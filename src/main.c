#include "badger/core.h"
#include "badger/fmt.h"
#include "badger/version.h"

#include <inttypes.h>
#include <stdlib.h>

int main(void)
{
    int32_t sum = {0};

    if (!tb_sum_i32(2, 3, &sum))
    {
        (void)tb_fmt_print_error("burrow: bootstrap sum failed\n");
        return EXIT_FAILURE;
    }
    (void)tb_fmt_print("Tenacious Badger %s: 2 + 3 = %" PRId32 "\n", TB_VERSION_STRING, sum);
    /* Formatting reports no failure; the stream does, once it is flushed. */
    if (!tb_fmt_flush())
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
