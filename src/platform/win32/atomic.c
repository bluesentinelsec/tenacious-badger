#include "badger/platform.h"

#include "badger/assert.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

TB_STATIC_ASSERT(sizeof(size_t) == sizeof(SIZE_T), "size_t and SIZE_T must be the same type size");
TB_STATIC_ASSERT(sizeof(uint32_t) == sizeof(LONG), "uint32_t and LONG must be the same size");
TB_STATIC_ASSERT(sizeof(size_t) == sizeof(PVOID), "a size_t is exchanged as a pointer is");

size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount)
{
    TB_ASSERT(atomic != NULL);
    /* The interlocked function returns the value from before the addition. */
    return (size_t)InterlockedExchangeAddSizeT(&atomic->value, amount) + amount;
}

size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount)
{
    TB_ASSERT(atomic != NULL);
    /* Adding the two's complement subtracts; unsigned arithmetic wraps by definition. */
    const size_t negated = (size_t)0 - amount;

    return (size_t)InterlockedExchangeAddSizeT(&atomic->value, negated) + negated;
}

/* A load is a read, and writes nothing.
 *
 * Every other operation here is an interlocked one, which on every Windows
 * processor is a full barrier: a write is in memory, for every thread to
 * see, before the thread that made it goes on. A read that the compiler may
 * not move or drop is then in step with them, and that is what the system's
 * "acquire" reads are: a plain read on x86, and a read with a barrier on
 * Arm.
 *
 * An interlocked operation that changes nothing would give the same answer.
 * It is a write to the processor, though, which takes the cache line away
 * from every other thread that is reading it. Threads that only look at a
 * value, as a thief looks at the ends of another worker's queue, would then
 * get in each other's way as much as threads that change it. */
size_t tb_platform_atomic_size_load(const tb_platform_atomic_size *atomic)
{
    TB_ASSERT(atomic != NULL);
    return (size_t)ReadSizeTAcquire(&atomic->value);
}

/* The system has no exchange for a SIZE_T by that name. A pointer is the
 * same size on every Windows, and its exchange moves the same bits. */
static PVOID *tb_win32_atomic_size_as_pointer(tb_platform_atomic_size *atomic)
{
    return (PVOID *)&atomic->value;
}

void tb_platform_atomic_size_store(tb_platform_atomic_size *atomic, size_t value)
{
    TB_ASSERT(atomic != NULL);
    (void)InterlockedExchangePointer(tb_win32_atomic_size_as_pointer(atomic), (PVOID)value);
}

size_t tb_platform_atomic_size_exchange(tb_platform_atomic_size *atomic, size_t value)
{
    TB_ASSERT(atomic != NULL);
    return (size_t)InterlockedExchangePointer(tb_win32_atomic_size_as_pointer(atomic),
                                              (PVOID)value);
}

bool tb_platform_atomic_size_compare_exchange(tb_platform_atomic_size *atomic, size_t *expected,
                                              size_t desired)
{
    size_t found = 0;

    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    /* The interlocked function returns the value it found, written or not. */
    found = (size_t)InterlockedCompareExchangePointer(tb_win32_atomic_size_as_pointer(atomic),
                                                      (PVOID)desired, (PVOID)*expected);
    if (found == *expected)
    {
        return true;
    }
    *expected = found;
    return false;
}

/* The interlocked functions for 32 bits take a signed value. The bits are the
 * same, and none of the operations here does arithmetic on them. */
static LONG *tb_win32_atomic_long(tb_platform_atomic_u32 *atomic)
{
    return (LONG *)&atomic->value;
}

uint32_t tb_platform_atomic_u32_load(const tb_platform_atomic_u32 *atomic)
{
    TB_ASSERT(atomic != NULL);
    /* Read as the signed value that the interlocked functions write. */
    return (uint32_t)ReadAcquire((const LONG *)&atomic->value);
}

void tb_platform_atomic_u32_store(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    TB_ASSERT(atomic != NULL);
    (void)InterlockedExchange(tb_win32_atomic_long(atomic), (LONG)value);
}

uint32_t tb_platform_atomic_u32_exchange(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    TB_ASSERT(atomic != NULL);
    return (uint32_t)InterlockedExchange(tb_win32_atomic_long(atomic), (LONG)value);
}

bool tb_platform_atomic_u32_compare_exchange(tb_platform_atomic_u32 *atomic, uint32_t *expected,
                                             uint32_t desired)
{
    uint32_t found = 0;

    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    /* The interlocked function returns the value it found, written or not. */
    found = (uint32_t)InterlockedCompareExchange(tb_win32_atomic_long(atomic), (LONG)desired,
                                                 (LONG)*expected);
    if (found == *expected)
    {
        return true;
    }
    *expected = found;
    return false;
}

void *tb_platform_atomic_pointer_load(const tb_platform_atomic_pointer *atomic)
{
    TB_ASSERT(atomic != NULL);
    return ReadPointerAcquire(&atomic->value);
}

void tb_platform_atomic_pointer_store(tb_platform_atomic_pointer *atomic, void *value)
{
    TB_ASSERT(atomic != NULL);
    (void)InterlockedExchangePointer(&atomic->value, value);
}

void *tb_platform_atomic_pointer_exchange(tb_platform_atomic_pointer *atomic, void *value)
{
    TB_ASSERT(atomic != NULL);
    return InterlockedExchangePointer(&atomic->value, value);
}

bool tb_platform_atomic_pointer_compare_exchange(tb_platform_atomic_pointer *atomic,
                                                 void **expected, void *desired)
{
    void *found = NULL;

    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    found = InterlockedCompareExchangePointer(&atomic->value, desired, *expected);
    if (found == *expected)
    {
        return true;
    }
    *expected = found;
    return false;
}
