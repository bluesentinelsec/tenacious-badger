#include "badger/system.h"

#include "not_implemented.h"

/* Standard C cannot count processors, and a guess of one would make every
 * machine look like the slowest. */

void tb_system_get_processors(tb_system_processors *processors)
{
    (void)processors;
    tb_default_not_implemented("platform.system", "tb_system_get_processors");
}

size_t tb_system_list_processors(tb_system_processor *processors, size_t capacity)
{
    (void)processors;
    (void)capacity;
    tb_default_not_implemented("platform.system", "tb_system_list_processors");
}
