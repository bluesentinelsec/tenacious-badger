/* The platform adapters state their contracts with TB_ASSERT, and a failed
 * one calls tb_assert_fail(), which the engine's core defines. The adapters'
 * tests are built from the adapter alone, without the core, so this file
 * defines it for them: a failed assertion becomes a trap of the test
 * framework, as it does in the engine's tests, and a case can check for it
 * with TB_TEST_EXPECT_TRAP. */
#include "badger/assert.h"

#include "tb_test.h"

#include <stdio.h>

/* Capacity of the trap message built from a failed assertion. */
#define TB_PLATFORM_TEST_MESSAGE_CAPACITY 512

void tb_assert_fail(const char *expression, const char *message, const char *file, int line,
                    const char *function)
{
    char text[TB_PLATFORM_TEST_MESSAGE_CAPACITY] = {0};

    (void)function;
    if (message != NULL)
    {
        (void)snprintf(text, sizeof(text), "assertion failed: %s (%s)", expression, message);
    }
    else
    {
        (void)snprintf(text, sizeof(text), "assertion failed: %s", expression);
    }
    tb_test_trap(file, line, text);
}

bool tb_assert_library_enabled(void)
{
    return TB_ASSERT_ENABLED != 0;
}
