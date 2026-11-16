#include "badger/platform.h"

#include "not_implemented.h"

/* An operation that is not one indivisible step would be worse than none: it
 * would work until two threads met in it. So these are not written in plain
 * C; each one reports that it is missing. */

#define TB_DEFAULT_ATOMIC_INTERFACE "platform.atomic"

size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount)
{
    (void)atomic;
    (void)amount;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_size_add");
}

size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount)
{
    (void)atomic;
    (void)amount;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_size_subtract");
}

size_t tb_platform_atomic_size_load(tb_platform_atomic_size *atomic)
{
    (void)atomic;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_size_load");
}

uint32_t tb_platform_atomic_u32_load(tb_platform_atomic_u32 *atomic)
{
    (void)atomic;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_u32_load");
}

void tb_platform_atomic_u32_store(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    (void)atomic;
    (void)value;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_u32_store");
}

uint32_t tb_platform_atomic_u32_exchange(tb_platform_atomic_u32 *atomic, uint32_t value)
{
    (void)atomic;
    (void)value;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_u32_exchange");
}

bool tb_platform_atomic_u32_compare_exchange(tb_platform_atomic_u32 *atomic, uint32_t *expected,
                                             uint32_t desired)
{
    (void)atomic;
    (void)expected;
    (void)desired;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE,
                               "tb_platform_atomic_u32_compare_exchange");
}

void *tb_platform_atomic_pointer_load(tb_platform_atomic_pointer *atomic)
{
    (void)atomic;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_pointer_load");
}

void tb_platform_atomic_pointer_store(tb_platform_atomic_pointer *atomic, void *value)
{
    (void)atomic;
    (void)value;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_pointer_store");
}

void *tb_platform_atomic_pointer_exchange(tb_platform_atomic_pointer *atomic, void *value)
{
    (void)atomic;
    (void)value;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE, "tb_platform_atomic_pointer_exchange");
}

bool tb_platform_atomic_pointer_compare_exchange(tb_platform_atomic_pointer *atomic,
                                                 void **expected, void *desired)
{
    (void)atomic;
    (void)expected;
    (void)desired;
    tb_default_not_implemented(TB_DEFAULT_ATOMIC_INTERFACE,
                               "tb_platform_atomic_pointer_compare_exchange");
}
