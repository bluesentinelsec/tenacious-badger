#include "badger/core.h"

#include <stdlib.h>

int main(void)
{
    int32_t result = {0};

    if (!tb_sum_i32(20, 22, &result) || result != 42)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
