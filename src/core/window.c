#include "badger/window.h"

const char *tb_window_result_name(tb_window_result result)
{
    switch (result)
    {
    case TB_WINDOW_OK:
        return "ok";
    case TB_WINDOW_UNAVAILABLE:
        return "unavailable";
    case TB_WINDOW_SYSTEM_ERROR:
        return "system error";
    }
    return "unknown result";
}
