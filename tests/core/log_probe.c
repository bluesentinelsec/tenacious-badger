#include "badger/log.h"
#include "tb_test.h"
TB_TEST(log_probe, stderr_record)
{
    tb_log *const log = tb_log_create();
    TB_TEST_REQUIRE_UINT(tb_log_printf(log, TB_LOG_INFO, "probe", "value %u", 42U), ==, TB_LOG_OK);
    TB_TEST_REQUIRE_UINT(tb_log_destroy(log), ==, TB_LOG_OK);
}
TB_TEST(log_probe, stderr_failure)
{
    tb_log *const log = tb_log_create();
    TB_TEST_REQUIRE_UINT(tb_log_printf(log, TB_LOG_INFO, "probe", "value"), ==, TB_LOG_IO);
    TB_TEST_REQUIRE_UINT(tb_log_flush(log), ==, TB_LOG_IO);
    TB_TEST_REQUIRE_UINT(tb_log_destroy(log), ==, TB_LOG_IO);
}
