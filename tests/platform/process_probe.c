/* Cases that move the probe's own standard streams, which the cases of
 * tb_tests cannot do; tests/tools/output_file_test.py runs each in its own
 * process, in an empty working directory, and reads what they wrote. */
#include "badger/fmt.h"
#include "badger/platform.h"
#include "badger/process.h"
#include "tb_test.h"

/* Every way the engine writes to the two streams, interleaved, so that the
 * file shows they arrive in the order they were written. */
TB_TEST(process_probe, both_streams_go_to_the_file)
{
    static const char native[] = "native error 3\n";

    TB_TEST_REQUIRE(tb_process_write_output_to("output.log"));
    (void)tb_fmt_print("output 1\n");
    (void)tb_fmt_print_error("error 2\n");
    tb_platform_write_error(native, sizeof(native) - 1);
    (void)tb_fmt_print("output 4\n");
}

/* A file that cannot be made leaves both streams where they were. */
TB_TEST(process_probe, a_file_that_cannot_be_made_moves_nothing)
{
    TB_TEST_REQUIRE(!tb_process_write_output_to("missing/output.log"));
    (void)tb_fmt_print("still on standard output\n");
    (void)tb_fmt_print_error("still on standard error\n");
}
