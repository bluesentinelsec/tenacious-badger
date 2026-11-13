#include "badger/platform.h"

/* The compiler's atomic builtins act on a plain object, so the public type
 * needs no compiler-specific qualifier. GCC and Clang both provide them. */

size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount)
{
    return __atomic_add_fetch(&atomic->value, amount, __ATOMIC_SEQ_CST);
}

size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount)
{
    return __atomic_sub_fetch(&atomic->value, amount, __ATOMIC_SEQ_CST);
}

size_t tb_platform_atomic_size_load(tb_platform_atomic_size *atomic)
{
    return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}
