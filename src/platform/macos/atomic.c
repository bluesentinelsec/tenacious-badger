#include "badger/platform.h"

#include "badger/assert.h"

/* The compiler's atomic builtins act on a plain object, so the public type
 * needs no compiler-specific qualifier. GCC and Clang both provide them. */

size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_add_fetch(&atomic->value, amount, __ATOMIC_SEQ_CST);
}

size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_sub_fetch(&atomic->value, amount, __ATOMIC_SEQ_CST);
}

size_t tb_platform_atomic_size_load(const tb_platform_atomic_size *atomic)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

void tb_platform_atomic_size_store(tb_platform_atomic_size *atomic, size_t value)
{
    TB_ASSERT(atomic != NULL);
    __atomic_store_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

size_t tb_platform_atomic_size_exchange(tb_platform_atomic_size *atomic, size_t value)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

bool tb_platform_atomic_size_compare_exchange(tb_platform_atomic_size *atomic, size_t *expected,
                                              size_t desired)
{
    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    /* The strong form: it fails only when the value is not the expected one. */
    return __atomic_compare_exchange_n(&atomic->value, expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

uint32_t tb_platform_atomic_u32_load(const tb_platform_atomic_u32 *atomic)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

void tb_platform_atomic_u32_store(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    TB_ASSERT(atomic != NULL);
    __atomic_store_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

uint32_t tb_platform_atomic_u32_exchange(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

bool tb_platform_atomic_u32_compare_exchange(tb_platform_atomic_u32 *atomic, uint32_t *expected,
                                             uint32_t desired)
{
    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    /* The strong form: it fails only when the value is not the expected one. */
    return __atomic_compare_exchange_n(&atomic->value, expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

void *tb_platform_atomic_pointer_load(const tb_platform_atomic_pointer *atomic)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

void tb_platform_atomic_pointer_store(tb_platform_atomic_pointer *atomic, void *value)
{
    TB_ASSERT(atomic != NULL);
    __atomic_store_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

void *tb_platform_atomic_pointer_exchange(tb_platform_atomic_pointer *atomic, void *value)
{
    TB_ASSERT(atomic != NULL);
    return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

bool tb_platform_atomic_pointer_compare_exchange(tb_platform_atomic_pointer *atomic,
                                                 void **expected, void *desired)
{
    TB_ASSERT(atomic != NULL);
    TB_ASSERT(expected != NULL);
    return __atomic_compare_exchange_n(&atomic->value, expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
