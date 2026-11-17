#include "crash_native.h"
#include "tb_test.h"
#include <stdint.h>
#include <stdlib.h>

TB_TEST(crash_fixture, a_passes)
{
    TB_TEST_EXPECT(true);
}
TB_TEST(crash_fixture, aborts)
{
    abort();
}
TB_TEST(crash_fixture, invalid_access)
{
    volatile uintptr_t address = 1;
    *(volatile unsigned char *)address = 42;
}
TB_TEST(crash_fixture, arithmetic_signal)
{
    /* ARM integer division need not fault. Raise the arithmetic signal directly
     * so native crash reporting is exercised on every supported architecture. */
    tb_test_fixture_arithmetic_fault();
}
