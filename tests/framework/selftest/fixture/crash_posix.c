#include "crash_native.h"
#include <signal.h>
void tb_test_fixture_arithmetic_fault(void)
{
    (void)raise(SIGFPE);
}
