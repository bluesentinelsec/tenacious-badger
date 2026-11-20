/* macOS keeps the system's libraries in /usr/lib and its frameworks in
 * /System/Library/Frameworks, and both in the dynamic linker's shared cache
 * rather than as files. A full path is given to dlopen(), which then never
 * searches DYLD_LIBRARY_PATH or the working directory. */
#include "../posix/library.h"
#include "../shared/library.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TB_MACOS_LIBRARIES "/usr/lib/"
#define TB_MACOS_FRAMEWORKS "/System/Library/Frameworks/"
#define TB_MACOS_FRAMEWORK_SUFFIX ".framework"

/* Whether `name` has the form "Name.framework/Name". */
static bool tb_macos_library_is_framework(const char *name)
{
    const char *const slash = strchr(name, '/');
    const size_t suffix = sizeof(TB_MACOS_FRAMEWORK_SUFFIX) - 1;
    size_t directory = 0;

    if (slash == NULL)
    {
        return false;
    }
    directory = (size_t)(slash - name);
    if (directory <= suffix || memcmp(slash - suffix, TB_MACOS_FRAMEWORK_SUFFIX, suffix) != 0)
    {
        return false;
    }
    /* The directory is named for the file inside it. */
    return strlen(slash + 1) == directory - suffix &&
           memcmp(name, slash + 1, directory - suffix) == 0 &&
           tb_native_library_is_file_name(slash + 1);
}

tb_library_result tb_posix_library_system_path(const char *name, char **path,
                                               tb_library_error *error)
{
    const char *directory = NULL;
    size_t first = 0;
    size_t second = 0;

    *path = NULL;
    if (tb_macos_library_is_framework(name))
    {
        directory = TB_MACOS_FRAMEWORKS;
    }
    else if (tb_native_library_is_file_name(name))
    {
        directory = TB_MACOS_LIBRARIES;
    }
    else
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not a file name, or a framework's Name.framework/Name");
        return TB_LIBRARY_INVALID;
    }
    first = strlen(directory);
    second = strlen(name);
    *path = malloc(first + second + 1);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (*path == NULL)
    {
        return TB_LIBRARY_NO_MEMORY;
    }
    /* coverage-on */
    memcpy(*path, directory, first);
    memcpy(*path + first, name, second + 1);
    return TB_LIBRARY_OK;
}
