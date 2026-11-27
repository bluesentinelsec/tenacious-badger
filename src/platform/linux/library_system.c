/* The dynamic linker finds a system library by its file name: in the
 * directories of LD_LIBRARY_PATH, of the engine library's run path, of
 * /etc/ld.so.cache, and in /lib and /usr/lib. A name with no '/' is never
 * looked for in the working directory. */
#include "library_linux.h"

#include "../posix/library.h"
#include "../shared/library.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

tb_library_result tb_posix_library_system_path(const char *name, char **path,
                                               tb_library_error *error)
{
    const size_t length = strlen(name);

    *path = NULL;
    if (!tb_native_library_is_file_name(name))
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not a file name");
        return TB_LIBRARY_INVALID;
    }
    *path = malloc(length + 1);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (*path == NULL)
    {
        return TB_LIBRARY_NO_MEMORY;
    }
    /* coverage-on */
    memcpy(*path, name, length + 1);
    return TB_LIBRARY_OK;
}

bool tb_linux_library_keep_loaded(const char *name)
{
    void *const handle = dlopen(name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD | RTLD_NODELETE);
    if (handle == NULL)
    {
        return false;
    }
    (void)dlclose(handle);
    return true;
}
