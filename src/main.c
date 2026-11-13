#include "badger/core.h"
#include "badger/version.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int32_t sum = {0};

    if (!tb_sum_i32(2, 3, &sum))
    {
        fputs("burrow: bootstrap sum failed\n", stderr);
        return EXIT_FAILURE;
    }
    if (printf("Tenacious Badger %s: 2 + 3 = %" PRId32 "\n", TB_VERSION_STRING, sum) < 0)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
