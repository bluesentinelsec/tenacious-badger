#include "badger/library.h"

const char *tb_library_result_name(tb_library_result result)
{
    switch (result)
    {
    case TB_LIBRARY_OK:
        return "ok";
    case TB_LIBRARY_INVALID:
        return "invalid argument";
    case TB_LIBRARY_UNAVAILABLE:
        return "library unavailable";
    case TB_LIBRARY_SYMBOL_MISSING:
        return "symbol missing";
    case TB_LIBRARY_NO_MEMORY:
        return "out of memory";
    }
    return "unknown result";
}
