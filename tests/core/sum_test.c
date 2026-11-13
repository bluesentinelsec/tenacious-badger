#include "badger/core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int tb_check_sum(int32_t left, int32_t right, int32_t expected)
{
    int32_t result = {0};

    if (!tb_sum_i32(left, right, &result) || result != expected)
    {
        fprintf(stderr, "sum(%" PRId32 ", %" PRId32 "): expected %" PRId32 ", got %" PRId32 "\n",
                left, right, expected, result);
        return 1;
    }
    return 0;
}

static int tb_check_overflow(int32_t left, int32_t right)
{
    enum
    {
        TB_TEST_OUTPUT_SENTINEL = 123
    };
    int32_t result = {0};

    result = TB_TEST_OUTPUT_SENTINEL;
    if (tb_sum_i32(left, right, &result) || result != TB_TEST_OUTPUT_SENTINEL)
    {
        fprintf(stderr, "sum(%" PRId32 ", %" PRId32 "): overflow must preserve the output\n", left,
                right);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = {0};

    failures += tb_check_sum(2, 3, 5);
    failures += tb_check_sum(0, 0, 0);
    failures += tb_check_sum(-2, -3, -5);
    failures += tb_check_sum(-2, 3, 1);
    failures += tb_check_sum(INT32_MAX, 0, INT32_MAX);
    failures += tb_check_sum(INT32_MIN, 0, INT32_MIN);
    failures += tb_check_sum(INT32_MAX - 1, 1, INT32_MAX);
    failures += tb_check_sum(INT32_MIN + 1, -1, INT32_MIN);
    failures += tb_check_sum(INT32_MIN, INT32_MAX, -1);
    failures += tb_check_sum(INT32_MAX, INT32_MIN, -1);
    failures += tb_check_overflow(INT32_MAX, 1);
    failures += tb_check_overflow(INT32_MIN, -1);
    failures += tb_check_overflow(INT32_MAX, INT32_MAX);
    failures += tb_check_overflow(INT32_MIN, INT32_MIN);
    failures += tb_check_overflow(1, INT32_MAX);
    failures += tb_check_overflow(-1, INT32_MIN);

    if (failures != 0)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
