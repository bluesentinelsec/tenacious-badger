/* Shared libraries through dlopen(), on Linux and macOS. Where the system's
 * libraries are is each system's own (library_system.c); a library beside
 * the executable or at a path is opened by its full path, which dlopen()
 * does not search for.
 *
 * Every library is opened with RTLD_NOW, so a symbol it needs and no loaded
 * library has fails the open, not a call in the middle of a frame; and with
 * RTLD_LOCAL, so its symbols do not take the place of another library's. */
#define _POSIX_C_SOURCE 200809L
#include "library.h"

#include "../shared/fs.h"
#include "../shared/library.h"
#include "badger/assert.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static tb_library_result tb_posix_library_out_of_memory(tb_library_error *error, const char *name)
{
    (void)tb_native_library_fail(error, TB_LIBRARY_NO_MEMORY, name);
    tb_native_library_say(error, "out of memory");
    return TB_LIBRARY_NO_MEMORY;
}

/* The path of a library beside the executable, from malloc. */
static tb_library_result tb_posix_library_application_path(const char *name, char **path,
                                                           tb_library_error *error)
{
    char *directory = NULL;
    size_t first = 0;
    size_t second = 0;

    *path = NULL;
    if (!tb_native_library_is_file_name(name))
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not a file name");
        return TB_LIBRARY_INVALID;
    }
    /* coverage-off: the executable's directory is always known */
    if (tb_native_fs_location(TB_FS_EXECUTABLE, &directory) != TB_FS_OK)
    {
        return tb_posix_library_out_of_memory(error, name);
    }
    /* coverage-on */
    first = strlen(directory);
    second = strlen(name);
    *path = malloc(first + second + 2);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (*path == NULL)
    {
        free(directory);
        return tb_posix_library_out_of_memory(error, name);
    }
    /* coverage-on */
    memcpy(*path, directory, first);
    /* The directory is given with its '/', but its root is "/" alone. */
    if (first == 0 || directory[first - 1] != '/')
    {
        (*path)[first] = '/';
        first++;
    }
    memcpy(*path + first, name, second + 1);
    free(directory);
    return TB_LIBRARY_OK;
}

/* A copy of an absolute path, from malloc. */
static tb_library_result tb_posix_library_exact_path(const char *name, char **path,
                                                     tb_library_error *error)
{
    const size_t length = strlen(name);

    *path = NULL;
    if (!tb_native_library_is_text(name))
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not UTF-8");
        return TB_LIBRARY_INVALID;
    }
    if (name[0] != '/')
    {
        (void)tb_native_library_fail(error, TB_LIBRARY_INVALID, name);
        tb_native_library_say(error, "not an absolute path");
        return TB_LIBRARY_INVALID;
    }
    *path = malloc(length + 1);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (*path == NULL)
    {
        return tb_posix_library_out_of_memory(error, name);
    }
    /* coverage-on */
    memcpy(*path, name, length + 1);
    return TB_LIBRARY_OK;
}

tb_library_result tb_library_open(const char *name, tb_library_search search, tb_library **library,
                                  tb_library_error *error)
{
    tb_library_result result = TB_LIBRARY_OK;
    tb_library *record = NULL;
    char *path = NULL;
    void *handle = NULL;

    TB_ASSERT(name != NULL);
    TB_ASSERT(library != NULL);
    TB_ASSERT(search == TB_LIBRARY_SEARCH_SYSTEM || search == TB_LIBRARY_SEARCH_APPLICATION ||
              search == TB_LIBRARY_SEARCH_PATH);
    *library = NULL;
    result = TB_LIBRARY_INVALID;
    switch (search)
    {
    case TB_LIBRARY_SEARCH_SYSTEM:
        result = tb_posix_library_system_path(name, &path, error);
        /* coverage-off: allocation fails only when the system is out of memory */
        if (result == TB_LIBRARY_NO_MEMORY)
        {
            result = tb_posix_library_out_of_memory(error, name);
        }
        /* coverage-on */
        break;
    case TB_LIBRARY_SEARCH_APPLICATION:
        result = tb_posix_library_application_path(name, &path, error);
        break;
    case TB_LIBRARY_SEARCH_PATH:
        result = tb_posix_library_exact_path(name, &path, error);
        break;
    }
    if (result != TB_LIBRARY_OK)
    {
        return result;
    }
    record = tb_native_library_record(name);
    /* coverage-off: allocation fails only when the system is out of memory */
    if (record == NULL)
    {
        result = tb_posix_library_out_of_memory(error, name);
        goto cleanup;
    }
    /* coverage-on */
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL)
    {
        /* The text is the calling thread's own, until its next call of the
         * dynamic linker; it is copied into the message at once. */
        result = tb_native_library_not_loaded(error, name, search, dlerror());
        goto cleanup;
    }
    record->handle = handle;
    *library = record;
    record = NULL;
    result = tb_native_library_succeed(error);
cleanup:
    free(record);
    free(path);
    return result;
}

void tb_library_close(tb_library *library)
{
    if (library == NULL)
    {
        return;
    }
    (void)dlclose(library->handle);
    free(library);
}

/* The address of an export, or NULL. A symbol whose address is zero is
 * taken for missing. */
static void *tb_posix_library_find(const tb_library *library, const char *name)
{
    TB_ASSERT(library != NULL);
    TB_ASSERT(name != NULL);
    return dlsym(library->handle, name);
}

tb_library_result tb_library_find_function(const tb_library *library, const char *name,
                                           tb_library_function *function, tb_library_error *error)
{
    void *const address = tb_posix_library_find(library, name);

    TB_ASSERT(function != NULL);
    *function = NULL;
    if (address == NULL)
    {
        return tb_native_library_missing(error, library, name);
    }
    /* dlsym() gives every symbol the type of data. POSIX requires that it
     * convert to a function pointer; standard C has no conversion between
     * them, so the bytes are copied. */
    TB_STATIC_ASSERT(sizeof(*function) == sizeof(address),
                     "data and function pointers differ in size");
    memcpy(function, &address, sizeof(*function));
    return tb_native_library_succeed(error);
}

tb_library_result tb_library_find_data(const tb_library *library, const char *name, void **data,
                                       tb_library_error *error)
{
    void *const address = tb_posix_library_find(library, name);

    TB_ASSERT(data != NULL);
    *data = NULL;
    if (address == NULL)
    {
        return tb_native_library_missing(error, library, name);
    }
    *data = address;
    return tb_native_library_succeed(error);
}
