#include "../../src/platform.h"
#include "tb_test.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static void existing_handler(int number)
{
    (void)number;
    _Exit(77);
}
int main(int argc, char **argv)
{
    /* --break must not open interactive OS dialogs in this child fixture. */
    tb_test_platform_disable_error_dialogs();
    if (argc > 1 && strcmp(argv[1], "--existing-handler") == 0)
    {
        (void)signal(SIGABRT, existing_handler);
        argv[1] = argv[0];
        return tb_test_main(argc - 1, argv + 1);
    }
    if (argc > 1 && strcmp(argv[1], "--after-run") == 0)
    {
        int result = {0};
        argv[1] = argv[0];
        result = tb_test_main(argc - 1, argv + 1);
        if (result != 0)
        {
            return result;
        }
        abort();
    }
    return tb_test_main(argc, argv);
}
