#include "badger/platform.h"

#include "badger/assert.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

TB_STATIC_ASSERT(sizeof(size_t) == sizeof(SIZE_T), "size_t and SIZE_T must be the same type size");
TB_STATIC_ASSERT(sizeof(uint32_t) == sizeof(LONG), "uint32_t and LONG must be the same size");

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

/* The interlocked functions for 32 bits take a signed value. The bits are the
 * same, and none of the operations here does arithmetic on them. */
static LONG *tb_win32_atomic_long(tb_platform_atomic_u32 *atomic)
{
    return (LONG *)&atomic->value;
}

uint32_t tb_platform_atomic_u32_load(tb_platform_atomic_u32 *atomic)
{
    /* Adding zero reads the value in step with the other operations. */
    return (uint32_t)InterlockedExchangeAdd(tb_win32_atomic_long(atomic), 0);
}

void tb_platform_atomic_u32_store(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    (void)InterlockedExchange(tb_win32_atomic_long(atomic), (LONG)value);
}

uint32_t tb_platform_atomic_u32_exchange(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    return (uint32_t)InterlockedExchange(tb_win32_atomic_long(atomic), (LONG)value);
}

bool tb_platform_atomic_u32_compare_exchange(tb_platform_atomic_u32 *atomic, uint32_t *expected,
                                             uint32_t desired)
{
    /* The interlocked function returns the value it found, written or not. */
    const uint32_t found = (uint32_t)InterlockedCompareExchange(tb_win32_atomic_long(atomic),
                                                                (LONG)desired, (LONG)*expected);

    if (found == *expected)
    {
        return true;
    }
    *expected = found;
    return false;
}

void *tb_platform_atomic_pointer_load(tb_platform_atomic_pointer *atomic)
{
    /* Writes NULL only where NULL already is, so it changes nothing, and
     * returns what is there in step with the other operations. */
    return InterlockedCompareExchangePointer(&atomic->value, NULL, NULL);
}

void tb_platform_atomic_pointer_store(tb_platform_atomic_pointer *atomic, void *value)
{
    (void)InterlockedExchangePointer(&atomic->value, value);
}

void *tb_platform_atomic_pointer_exchange(tb_platform_atomic_pointer *atomic, void *value)
{
    return InterlockedExchangePointer(&atomic->value, value);
}

bool tb_platform_atomic_pointer_compare_exchange(tb_platform_atomic_pointer *atomic,
                                                 void **expected, void *desired)
{
    void *const found = InterlockedCompareExchangePointer(&atomic->value, desired, *expected);

    if (found == *expected)
    {
        return true;
    }
    *expected = found;
    return false;
}
