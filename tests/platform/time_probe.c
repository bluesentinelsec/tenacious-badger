#include "badger/platform.h"

#include <stdint.h>
#include <stdlib.h>

int main(void)
{
    uint64_t first = {0};
    uint64_t second = {0};

    first = tb_platform_ticks();
    second = tb_platform_ticks();
    if (second < first)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
