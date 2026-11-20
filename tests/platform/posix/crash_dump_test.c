/* On Linux and macOS the system writes core files, and the engine writes no
 * dump of its own: tb_crash_write_dumps() says so, whatever it is given. */
#include "badger/crash.h"

#include "tb_test.h"

TB_TEST(platform_crash_dump, the_system_writes_core_files_instead)
{
    TB_TEST_EXPECT(!tb_crash_write_dumps(NULL));
    TB_TEST_EXPECT(!tb_crash_write_dumps("."));
}
