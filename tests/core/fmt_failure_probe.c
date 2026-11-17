#include "badger/fmt.h"
#include "tb_test.h"

/* The parent gives one standard stream a read-only file handle. The other
 * remains writable, and carries proof that the case reached its checks. */
TB_TEST(fmt_failure_probe, stdout_refuses_output)
{
    TB_TEST_REQUIRE(tb_fmt_flush());
    (void)tb_fmt_print("this write must fail");
    TB_TEST_REQUIRE(!tb_fmt_flush());
    TB_TEST_REQUIRE(!tb_fmt_flush());
    (void)tb_fmt_print_error("stdout-failure-detected\n");
    TB_TEST_REQUIRE(tb_fmt_flush_error());
}

TB_TEST(fmt_failure_probe, stderr_refuses_output)
{
    TB_TEST_REQUIRE(tb_fmt_flush());
    (void)tb_fmt_print_error("this write must fail");
    TB_TEST_REQUIRE(!tb_fmt_flush());
    TB_TEST_REQUIRE(!tb_fmt_flush());
    (void)tb_fmt_print("stderr-failure-detected\n");
    TB_TEST_REQUIRE(!tb_fmt_flush_error());
}
