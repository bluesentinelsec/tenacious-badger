#include "crash_native.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
void tb_test_fixture_arithmetic_fault(void)
{
    RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_NONCONTINUABLE, 0, NULL);
}
