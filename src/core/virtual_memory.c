/* What the engine adds to the virtual memory interface without the platform's
 * help. The interface itself is implemented by each platform adapter. */
#include "badger/virtual_memory.h"

const char *tb_vm_result_name(tb_vm_result result)
{
    switch (result)
    {
    case TB_VM_OK:
        return "ok";
    case TB_VM_INVALID:
        return "invalid argument";
    case TB_VM_NO_MEMORY:
        return "out of memory";
    case TB_VM_UNAVAILABLE:
        return "unavailable";
    case TB_VM_OS_ERROR:
        return "operating system error";
    }
    return "unknown result";
}
