#include "engine_test.h"

#include "tb_test.h"

#include <stdio.h>

/* Capacity of the trap message built from a failed assertion. */
#define TB_ENGINE_TEST_MESSAGE_CAPACITY 512

static tb_assert_failure tb_engine_test_last_failure = {0};

const tb_assert_failure *tb_engine_test_last_assert_failure(void)
{
    return &tb_engine_test_last_failure;
}

/* Turns a failed engine assertion into a test framework trap. It leaves the
 * engine by a non-local jump, as tb_test_trap() documents. */
static void tb_engine_test_report_assert_failure(const tb_assert_failure *failure, void *context)
{
    char message[TB_ENGINE_TEST_MESSAGE_CAPACITY] = {0};

    (void)context;
    tb_engine_test_last_failure = *failure;
    if (failure->message != NULL)
    {
        (void)snprintf(message, sizeof(message), "assertion failed: %s (%s)", failure->expression,
                       failure->message);
    }
    else
    {
        (void)snprintf(message, sizeof(message), "assertion failed: %s", failure->expression);
    }
    tb_test_trap(failure->file, failure->line, message);
}

void tb_engine_test_install_assert_handler(void)
{
    tb_assert_set_handler(tb_engine_test_report_assert_failure, NULL);
}

int main(int argc, char **argv)
{
    tb_engine_test_install_assert_handler();
    return tb_test_main(argc, argv);
}
