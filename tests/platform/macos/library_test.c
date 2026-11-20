/* The macOS side of badger/library.h: a library of /usr/lib, and a
 * framework of /System/Library/Frameworks, both of which live in the
 * dynamic linker's shared cache rather than as files. */
#define _DARWIN_C_SOURCE
#include "badger/library.h"

#include "tb_test.h"

#include <sys/types.h>
#include <unistd.h>

typedef pid_t get_process_id_function(void);

TB_TEST(platform_library_macos, a_system_library_loads_from_usr_lib)
{
    tb_library *library = NULL;
    tb_library_function function = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_result result = TB_LIBRARY_OK;

    result = tb_library_open("libSystem.B.dylib", TB_LIBRARY_SEARCH_SYSTEM, &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(library, "getpid", &function, NULL), ==,
                         TB_LIBRARY_OK);
    TB_TEST_EXPECT_INT(((get_process_id_function *)function)(), ==, getpid());
    tb_library_close(library);
}

TB_TEST(platform_library_macos, a_framework_loads_by_its_name)
{
    tb_library *library = NULL;
    tb_library_function function = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_result result = TB_LIBRARY_OK;

    result = tb_library_open("CoreFoundation.framework/CoreFoundation", TB_LIBRARY_SEARCH_SYSTEM,
                             &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(tb_library_find_function(library, "CFRelease", &function, NULL), ==,
                        TB_LIBRARY_OK);
    tb_library_close(library);
}

/* A framework's file is named for its directory; any other path is no
 * system library's name. */
TB_TEST(platform_library_macos, a_framework_name_must_match_its_file)
{
    static const char *const names[] = {"CoreFoundation.framework/Other", ".framework/",
                                        "CoreFoundation.framework/", "Versions/A/CoreFoundation",
                                        "CoreFoundation.framework/Versions/CoreFoundation"};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        tb_library *library = NULL;
        TB_TEST_CONTEXT("%s", names[index]);
        TB_TEST_EXPECT_UINT(tb_library_open(names[index], TB_LIBRARY_SEARCH_SYSTEM, &library, NULL),
                            ==, TB_LIBRARY_INVALID);
        TB_TEST_EXPECT_POINTER(library, ==, NULL);
    }
}
