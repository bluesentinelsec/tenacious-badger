#include "badger/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

_Static_assert(sizeof(size_t) == sizeof(SIZE_T), "size_t and SIZE_T must be the same type size");

size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount)
{
    /* The interlocked function returns the value from before the addition. */
    return (size_t)InterlockedExchangeAddSizeT(&atomic->value, amount) + amount;
}

size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount)
{
    /* Adding the two's complement subtracts; unsigned arithmetic wraps by definition. */
    const size_t negated = (size_t)0 - amount;

    return (size_t)InterlockedExchangeAddSizeT(&atomic->value, negated) + negated;
}

size_t tb_platform_atomic_size_load(tb_platform_atomic_size *atomic)
{
    return (size_t)InterlockedExchangeAddSizeT(&atomic->value, 0);
}
