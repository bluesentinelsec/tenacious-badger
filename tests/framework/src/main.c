#include "tb_test.h"

/* Default entry point of a test executable. Supply the MAIN argument of
 * tb_add_test_executable to prepare the process before the cases run. */
int main(int argc, char **argv)
{
    return tb_test_main(argc, argv);
}
