/* The Windows side of badger/library.h: a library of the system directory,
 * and paths as Windows writes them. */
#include "badger/file.h"
#include "badger/library.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef DWORD WINAPI current_process_id_function(void);

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
    {
        length++;
    }
    return length;
}

TB_TEST(platform_library_win32, a_system_library_loads_and_its_function_is_the_systems)
{
    tb_library *library = NULL;
    tb_library_function function = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_result result = TB_LIBRARY_OK;

    result = tb_library_open("kernel32.dll", TB_LIBRARY_SEARCH_SYSTEM, &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(library, "GetCurrentProcessId", &function, NULL),
                         ==, TB_LIBRARY_OK);
    TB_TEST_EXPECT_UINT(((current_process_id_function *)function)(), ==, GetCurrentProcessId());
    tb_library_close(library);
}

/* The fixture's path, with `separator` between its components, and with or
 * without its ".dll". */
static void fixture_path(char *path, size_t capacity, char separator, bool extension)
{
    size_t required = 0;
    size_t length = 0;
    const char *const name = TB_LIBRARY_FIXTURE_FILE;
    const size_t name_length = text_length(name);

    TB_TEST_REQUIRE_UINT(tb_fs_get_location(TB_FS_EXECUTABLE, path, capacity, &required), ==,
                         TB_FS_OK);
    length = text_length(path);
    TB_TEST_REQUIRE(length + name_length + 1 <= capacity);
    memcpy(path + length, name, name_length + 1);
    for (size_t index = 0; path[index] != '\0'; index++)
    {
        if (path[index] == '\\' || path[index] == '/')
        {
            path[index] = separator;
        }
    }
    if (!extension)
    {
        path[text_length(path) - (sizeof(".dll") - 1)] = '\0';
    }
}

TB_TEST(platform_library_win32, a_path_may_be_written_with_forward_slashes)
{
    char path[4096] = {0};
    tb_library *library = NULL;
    tb_library_error error = TB_LIBRARY_ERROR_INIT;
    tb_library_result result = TB_LIBRARY_OK;

    fixture_path(path, sizeof(path), '/', true);
    result = tb_library_open(path, TB_LIBRARY_SEARCH_PATH, &library, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_EXPECT_UINT(result, ==, TB_LIBRARY_OK);
    tb_library_close(library);
}

/* LoadLibrary adds ".dll" to a name without an extension; an exact path
 * gets nothing added. */
TB_TEST(platform_library_win32, an_exact_path_gets_no_extension_added)
{
    char path[4096] = {0};
    tb_library *library = NULL;

    fixture_path(path, sizeof(path), '\\', false);
    TB_TEST_EXPECT_UINT(tb_library_open(path, TB_LIBRARY_SEARCH_PATH, &library, NULL), ==,
                        TB_LIBRARY_UNAVAILABLE);
    TB_TEST_EXPECT_POINTER(library, ==, NULL);
}
