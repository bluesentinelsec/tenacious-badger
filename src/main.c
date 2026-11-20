#include "badger/core.h"
#include "badger/crash.h"
#include "badger/fmt.h"
#include "badger/version.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>

int main(void)
{
    int32_t sum = {0};

    /* A crash is reported with its stack, wherever it happens. Burrow has a C
     * library of its own beside the engine's, so abort() is caught here too. */
    tb_crash_catch();
    (void)signal(SIGABRT, tb_crash_report_abort);
    /* coverage-off: 2 + 3 cannot overflow; the result of a fallible function is checked all the
     * same */
    if (!tb_sum_i32(2, 3, &sum))
    {
        (void)tb_fmt_print_error("burrow: bootstrap sum failed\n");
        return EXIT_FAILURE;
    }
    /* coverage-on */
    (void)tb_fmt_print("Tenacious Badger %s: 2 + 3 = %" PRId32 "\n", TB_VERSION_STRING, sum);
    /* Formatting reports no failure; the stream does, once it is flushed. */
    if (!tb_fmt_flush())
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
