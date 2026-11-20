/* The Linux side of badger/library.h: a library the dynamic linker finds by
 * its versioned name. */
#define _POSIX_C_SOURCE 200809L
#include "badger/library.h"

#include "tb_test.h"

#include <sys/types.h>
#include <unistd.h>

typedef pid_t get_process_id_function(void);

TB_TEST(platform_library_linux, a_system_library_loads_by_its_versioned_name)
{
    tb_library *library = NULL;
    tb_library_function function = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_result result = TB_LIBRARY_OK;

    result = tb_library_open("libc.so.6", TB_LIBRARY_SEARCH_SYSTEM, &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(library, "getpid", &function, NULL), ==,
                         TB_LIBRARY_OK);
    TB_TEST_EXPECT_INT(((get_process_id_function *)function)(), ==, getpid());
    tb_library_close(library);
}
