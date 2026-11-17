#include "badger/stack.h"

#include "not_implemented.h"

size_t tb_stack_capture(void **frames, size_t capacity, size_t skip)
{
    (void)frames;
    (void)capacity;
    (void)skip;
    tb_default_not_implemented("platform.stack", "tb_stack_capture");
}

void tb_stack_describe(const void *address, tb_stack_location *location)
{
    (void)address;
    (void)location;
    tb_default_not_implemented("platform.stack", "tb_stack_describe");
}
