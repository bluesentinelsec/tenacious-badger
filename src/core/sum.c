#include "badger/core.h"

#include "badger/assert.h"

bool tb_sum_i32(int32_t left, int32_t right, int32_t *result)
{
    TB_ASSERT(result != NULL);
    if (right > 0 && left > INT32_MAX - right)
    {
        return false;
    }
    if (right < 0 && left < INT32_MIN - right)
    {
        return false;
    }
    *result = left + right;
    return true;
}
